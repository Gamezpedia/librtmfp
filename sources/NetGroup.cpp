/*
Copyright 2016 Thomas Jammet
mathieu.poux[a]gmail.com
jammetthomas[a]gmail.com

This file is part of Librtmfp.

Librtmfp is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Librtmfp is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with Librtmfp.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "NetGroup.h"
#include "P2PSession.h"
#include "GroupStream.h"
#include "librtmfp.h"

using namespace Mona;
using namespace std;

#if defined(_WIN32)
	#define sscanf sscanf_s
#endif

// Peer instance in the heard list
class GroupNode : public virtual Object {
public:
	GroupNode(const char* rawPeerId, const string& groupId, const PEER_LIST_ADDRESS_TYPE& listAddresses, const SocketAddress& host, UInt64 timeElapsed) :
		rawId(rawPeerId, PEER_ID_SIZE + 2), groupAddress(groupId), addresses(listAddresses), hostAddress(host), lastGroupReport(((UInt64)Time::Now()) - timeElapsed) {}

	// Return the size of peer addresses for Group Report 
	UInt32	addressesSize() {
		UInt32 size = hostAddress.host().size() + 4; // +4 for 0A, address type and port
		for (auto itAddress : addresses)
			if (itAddress.second != RTMFP::ADDRESS_LOCAL)
				size += itAddress.first.host().size() + 3; // +3 for address type and port
		return size;
	}

	string rawId;
	string groupAddress;
	PEER_LIST_ADDRESS_TYPE addresses;
	SocketAddress hostAddress;
	UInt64 lastGroupReport; // Time in msec of last Group report received
};

const string& NetGroup::GetGroupAddressFromPeerId(const char* rawId, std::string& groupAddress) {
	
	static UInt8 tmp[PEER_ID_SIZE];
	EVP_Digest(rawId, PEER_ID_SIZE+2, tmp, NULL, EVP_sha256(), NULL);
	Util::FormatHex(tmp, PEER_ID_SIZE, groupAddress);
	TRACE("Group address : ", groupAddress)
	return groupAddress;
}

double NetGroup::estimatedPeersCount() {

	if (_mapGroupAddress.size() < 4)
		return _mapGroupAddress.size();

	// First get the neighbors N-2 and N+2
	auto itFirst = _mapGroupAddress.lower_bound(_myGroupAddress);
	auto itLast = itFirst;
	if (itFirst == _mapGroupAddress.end()) {
		itFirst = --(--(_mapGroupAddress.end()));
		itLast = ++(_mapGroupAddress.begin());
	}
	else {

		if (itFirst->first > _myGroupAddress) {  // Current == N+1?
			if (--itFirst == _mapGroupAddress.end())
				itFirst = --(_mapGroupAddress.end());
		} else if (++itLast == _mapGroupAddress.end())  // Current == N-1
			itLast = _mapGroupAddress.begin();

		if (--itFirst == _mapGroupAddress.end())
			itFirst = --(_mapGroupAddress.end());
		if (++itLast == _mapGroupAddress.end()) 
			itLast = _mapGroupAddress.begin();
	}
	
	TRACE("First peer (N-2) = ", itFirst->first)
	TRACE("Last peer (N+2) = ", itLast->first)

	UInt64 valFirst = 0, valLast = 0;
	sscanf(itFirst->first.substr(0, 16).c_str(), "%llx", &valFirst);
	sscanf(itLast->first.substr(0, 16).c_str(), "%llx", &valLast);

	// Then calculate the total	
	if (valLast > valFirst)
		return (MAX_PEER_COUNT / (double(valLast - valFirst) / 4)) + 1;
	else
		return (MAX_PEER_COUNT / (double(valLast - valFirst + MAX_PEER_COUNT) / 4)) + 1;
}

UInt32 NetGroup::targetNeighborsCount() {
	double memberCount = estimatedPeersCount();
	UInt32 targetNeighbor = (UInt32)(2 * log2(memberCount)) + 13;
	TRACE("estimatedMemberCount : ", memberCount, " ; targetNeighbor : ", targetNeighbor)

	return targetNeighbor;
}

NetGroup::NetGroup(const string& groupId, const string& groupTxt, const string& streamName, RTMFPSession& conn, RTMFPGroupConfig* parameters) : groupParameters(parameters),
	idHex(groupId), idTxt(groupTxt), stream(streamName), _conn(conn), _pListener(NULL), _groupMediaPublisher(_mapGroupMedias.end()) {
	onNewMedia = [this](const string& peerId, shared_ptr<PeerMedia>& pPeerMedia, const string& streamName, const string& streamKey, PacketReader& packet) {

		shared_ptr<RTMFPGroupConfig> pParameters(new RTMFPGroupConfig());
		memcpy(pParameters.get(), groupParameters, sizeof(RTMFPGroupConfig)); // TODO: make a initializer
		ReadGroupConfig(pParameters, packet);  // TODO: check groupParameters

		// We do not accept peer if they are not in the best list (TODO: not sure it is true anymore)
		/*if (!_bestList.empty() && _bestList.find(peerId) == _bestList.end()) {
			DEBUG("Best Peer - peer ", peerId, " media subscription rejected, not in the Best List")
			return false;
		}*/

		if (streamName != stream) {
			INFO("New stream available in the group but not registered : ", streamName)
			return false;
		}

		// Create the Group Media if it does not exists
		auto itGroupMedia = _mapGroupMedias.lower_bound(streamKey);
		if (itGroupMedia == _mapGroupMedias.end() || itGroupMedia->first != streamKey) {
			itGroupMedia = _mapGroupMedias.emplace_hint(itGroupMedia, piecewise_construct, forward_as_tuple(streamKey), forward_as_tuple(_conn.poolBuffers(), stream, streamKey, pParameters));
			itGroupMedia->second.subscribe(onGroupPacket);
			DEBUG("Creation of GroupMedia ", itGroupMedia->second.id," for the stream ", stream, " :\n", Util::FormatHex(BIN streamKey.data(), streamKey.size(), LOG_BUFFER))
		}
		
		// And finally try to add the peer and send the GroupMedia subscription
		itGroupMedia->second.addPeer(peerId, pPeerMedia);
		return true;
	};
	onGroupReport = [this](P2PSession* pPeer, PacketReader& packet, bool sendMediaSubscription) {
		
		auto itNode = _mapHeardList.find(pPeer->peerId);
		if (itNode != _mapHeardList.end())
			itNode->second.lastGroupReport = Time::Now(); // Record the time of last Group Report received to build our Group Report

		// If there are new peers : manage the best list
		if (readGroupReport(packet))
			updateBestList();

		// First Viewer = > create listener
		if (_groupMediaPublisher != _mapGroupMedias.end() && !_pListener) {
			Exception ex;
			if (!(_pListener = _conn.startListening<GroupListener>(ex, stream, idTxt))) {
				WARN(ex.error()) // TODO : See if we can send a specific answer
				return;
			}
			INFO("First viewer play request, starting to play Stream ", stream)
			_pListener->OnMedia::subscribe(_groupMediaPublisher->second.onMedia);
			_conn.publishReady = true; // A peer is connected : unlock the possible blocking RTMFP_PublishP2P function
		}

		if (!pPeer->groupReportInitiator) {
			sendGroupReport(pPeer, false);
			_lastReport.update();
		}
		else
			pPeer->groupReportInitiator = false;

		// Send the Group Media Subscription if not already sent
		if (sendMediaSubscription && (_bestList.empty() || _bestList.find(pPeer->peerId) != _bestList.end())) {
			for (auto& itGroupMedia : _mapGroupMedias) {
				if (itGroupMedia.second.groupParameters->isPublisher || itGroupMedia.second.hasFragments()) {
					auto pPeerMedia = pPeer->getPeerMedia(itGroupMedia.first);
					itGroupMedia.second.sendGroupMedia(pPeerMedia);
				}
			}
		}
	};
	onGroupBegin = [this](P2PSession* pPeer) {

		 // When we receive the 0E NetGroup message type we must send the group report if not already sent
		auto itNode = _mapHeardList.find(pPeer->peerId);
		if (itNode == _mapHeardList.end() || pPeer->groupFirstReportSent)
			return;

		sendGroupReport(pPeer, true);
		_lastReport.update();
	};
	onGroupPacket = [this](UInt32 time, const UInt8* data, UInt32 size, double lostRate, bool audio) {
		_conn.pushMedia(stream, time, data, size, lostRate, audio);
	};
	onPeerClose = [this](const string& peerId) {
		removePeer(peerId);
	};
	onGroupAskClose = [this](const string& peerId) {
		if (_bestList.empty())
			return true; // do not disconnect peer if we have not calculated the best list (can it happen?)

		return _bestList.find(peerId) != _bestList.end(); // if peer is not in the Best list return False tu close the main flow, otherwise keep connection open
	};

	GetGroupAddressFromPeerId(STR _conn.rawId(), _myGroupAddress);

	// If Publisher create a new GroupMedia
	if (groupParameters->isPublisher) {

		// Generate the stream key
		string streamKey("\x21\x01");
		streamKey.resize(0x22);
		Util::Random(BIN streamKey.data() + 2, 0x20); // random serie of 32 bytes

		shared_ptr<RTMFPGroupConfig> pParameters(new RTMFPGroupConfig());
		memcpy(pParameters.get(), groupParameters, sizeof(RTMFPGroupConfig)); // TODO: make a initializer
		_groupMediaPublisher = _mapGroupMedias.emplace(piecewise_construct, forward_as_tuple(streamKey), forward_as_tuple(_conn.poolBuffers(), stream, streamKey, pParameters)).first;
		_groupMediaPublisher->second.subscribe(onGroupPacket);
	}
}

NetGroup::~NetGroup() {
}

void NetGroup::stopListener() {
	if (_pListener) {
		if (_groupMediaPublisher != _mapGroupMedias.end())
			_pListener->OnMedia::unsubscribe(_groupMediaPublisher->second.onMedia);
		_groupMediaPublisher = _mapGroupMedias.end();
		_conn.stopListening(idTxt);
		_pListener = NULL;
	}
}

void NetGroup::close() {

	stopListener();

	for (auto& itGroupMedia : _mapGroupMedias) {
		itGroupMedia.second.unsubscribe(onGroupPacket);
	}
	_mapGroupMedias.clear();

	MAP_PEERS_ITERATOR_TYPE itPeer = _mapPeers.begin();
	while (itPeer != _mapPeers.end())
		removePeer(itPeer++); // (doesn't delete peer from the heard list but we don't care)
}

void NetGroup::addPeer2HeardList(const string& peerId, const char* rawId, const PEER_LIST_ADDRESS_TYPE& listAddresses, const SocketAddress& hostAddress, UInt64 timeElapsed) {

	auto it = _mapHeardList.lower_bound(peerId);
	if (it != _mapHeardList.end() && it->first == peerId) {
		DEBUG("The peer ", peerId, " is already known")
		return;
	}

	string groupAddress;
	_mapGroupAddress.emplace(GetGroupAddressFromPeerId(rawId, groupAddress), peerId);
	it = _mapHeardList.emplace_hint(it, piecewise_construct, forward_as_tuple(peerId.c_str()), forward_as_tuple(rawId, groupAddress, listAddresses, hostAddress, timeElapsed));
	DEBUG("Peer ", it->first, " added to heard list")
}

bool NetGroup::addPeer(const string& peerId, shared_ptr<P2PSession> pPeer) {

	if (_mapHeardList.find(peerId) == _mapHeardList.end()) {
		ERROR("Unknown peer to add : ", peerId)
		return false;
	}

	auto it = _mapPeers.lower_bound(peerId);
	if (it != _mapPeers.end() && it->first == peerId) {
		ERROR("Unable to add the peer ", peerId, ", it already exists")
		return false;
	}
	DEBUG("Adding the peer ", peerId, " to the Best List")
	_mapPeers.emplace_hint(it, peerId, pPeer);

	pPeer->OnNewMedia::subscribe(onNewMedia);
	pPeer->OnPeerGroupReport::subscribe(onGroupReport);
	pPeer->OnPeerGroupBegin::subscribe(onGroupBegin);
	pPeer->OnPeerClose::subscribe(onPeerClose);
	pPeer->OnPeerGroupAskClose::subscribe(onGroupAskClose);

	buildBestList(_myGroupAddress, _bestList); // rebuild the best list to know if the peer is in it
	return true;
}

void NetGroup::removePeer(const string& peerId) {

	auto itPeer = _mapPeers.find(peerId);
	if (itPeer == _mapPeers.end())
		DEBUG("The peer ", peerId, " is already removed from the Best list")
	else
		removePeer(itPeer);
}

void NetGroup::removePeer(MAP_PEERS_ITERATOR_TYPE itPeer) {
	DEBUG("Deleting peer ", itPeer->first, " from the NetGroup Best List")

	itPeer->second->OnNewMedia::unsubscribe(onNewMedia);
	itPeer->second->OnPeerGroupReport::unsubscribe(onGroupReport);
	itPeer->second->OnPeerGroupBegin::unsubscribe(onGroupBegin);
	itPeer->second->OnPeerClose::unsubscribe(onPeerClose);
	itPeer->second->OnPeerGroupAskClose::unsubscribe(onGroupAskClose);
	_mapPeers.erase(itPeer);
}

bool NetGroup::checkPeer(const string& peerId) {

	return _mapPeers.find(peerId) == _mapPeers.end();
}

void NetGroup::manage() {

	// Manage the Best list
	if (_lastBestCalculation.isElapsed(NETGROUP_BEST_LIST_DELAY))
		updateBestList();

	// Send the Group Report message (0A) to a random connected peer
	if (_lastReport.isElapsed(NETGROUP_REPORT_DELAY)) {

		auto itRandom = _mapPeers.begin();
		if (RTMFP::getRandomIt<MAP_PEERS_TYPE, MAP_PEERS_ITERATOR_TYPE>(_mapPeers, itRandom, [](const MAP_PEERS_ITERATOR_TYPE it) { return it->second->status == RTMFP::CONNECTED; }))
			sendGroupReport(itRandom->second.get(), true);

		// Clean the Heard List from old peers
		Int64 now = Time::Now();
		auto itHeardList = _mapHeardList.begin();
		while (itHeardList != _mapHeardList.end()) {
			if ((_mapPeers.find(itHeardList->first) == _mapPeers.end()) && now > itHeardList->second.lastGroupReport && ((now - itHeardList->second.lastGroupReport) > NETGROUP_PEER_TIMEOUT)) {
				DEBUG("Peer ", itHeardList->first, " timeout (", NETGROUP_PEER_TIMEOUT, "ms elapsed) - deleting from the heard list...")
				auto itGroupAddress = _mapGroupAddress.find(itHeardList->second.groupAddress);
				if (itGroupAddress == _mapGroupAddress.end())
					WARN("Unable to find peer ", itHeardList->first, " in the map of Group Addresses") // should not happen
				else
					_mapGroupAddress.erase(itGroupAddress);
				_mapHeardList.erase(itHeardList++);
				continue;
			}
			++itHeardList;
		}

		_lastReport.update();
	}

	// Manage all group medias
	for (auto& itGroupMedia : _mapGroupMedias)
		itGroupMedia.second.manage();
}

void NetGroup::updateBestList() {

	buildBestList(_myGroupAddress, _bestList);
	manageBestConnections();
	_lastBestCalculation.update();
}

void NetGroup::buildBestList(const string& groupAddress, set<string>& bestList) {
	bestList.clear();

	// Find the 6 closest peers
	if (_mapGroupAddress.size() <= 6) {
		for (auto it : _mapGroupAddress)
			bestList.emplace(it.second);
	}
	else { // More than 6 peers

		// First we search the first of the 6 peers
		map<string, string>::iterator itFirst = _mapGroupAddress.lower_bound(groupAddress);
		if (itFirst == _mapGroupAddress.end())
			itFirst = --(_mapGroupAddress.end());
		for (int i = 0; i < 2; i++) {
			if (--itFirst == _mapGroupAddress.end()) // if we reach the first peer we restart from the end
				itFirst = --(_mapGroupAddress.end());
		}

		for (int j = 0; j < 6; j++) {
			bestList.emplace(itFirst->second);

			if (++itFirst == _mapGroupAddress.end()) // if we reach the end we restart from the beginning
				itFirst = _mapGroupAddress.begin();
		}
	}

	// Find the 6 lowest latency
	if (_mapGroupAddress.size() > 6) {
		deque<shared_ptr<P2PSession>> queueLatency;
		if (!_mapPeers.empty()) {
			for (auto it : _mapPeers) { // First, order the peers by latency
				UInt16 latency = it.second->latency();
				auto it2 = queueLatency.begin();
				while (it2 != queueLatency.end() && (*it2)->latency() < latency)
					++it2;
				queueLatency.emplace(it2, it.second);
			}
			auto itLatency = queueLatency.begin();
			int i = 0;
			do {
				if (bestList.emplace((*itLatency)->peerId).second)
					i++;
			} while (++itLatency != queueLatency.end() && i < 6);
		}

		// Add one random peer
		if (_mapGroupAddress.size() > bestList.size()) {

			auto itRandom = _mapGroupAddress.begin();
			if (RTMFP::getRandomIt<map<string, string>, map<string, string>::iterator>(_mapGroupAddress, itRandom, [bestList](const map<string, string>::iterator& it) { return bestList.find(it->second) != bestList.end(); }))
				bestList.emplace(itRandom->second);
		}

		// Find 2 log(N) peers with location + 1/2, 1/4, 1/8 ...
		UInt32 bests = bestList.size(), estimatedCount = targetNeighborsCount();
		if (_mapGroupAddress.size() > bests && estimatedCount > bests) {
			UInt32 count = estimatedCount - bests;
			if (count > _mapGroupAddress.size() - bests)
				count = _mapGroupAddress.size() - bests;

			auto itNode = _mapGroupAddress.lower_bound(groupAddress);
			UInt32 rest = (_mapGroupAddress.size() / 2) - 1;
			UInt32 step = rest / (2 * count);
			for (; count > 0; count--) {
				if (distance(itNode, _mapGroupAddress.end()) <= step) {
					itNode = _mapGroupAddress.begin();
				}
				advance(itNode, step);
				while (!bestList.emplace(itNode->second).second) { // If not added go to next
					if (++itNode == _mapGroupAddress.end())
						itNode = _mapGroupAddress.begin();
				}
			}
		}
	}

	if (bestList == _bestList && _mapPeers.size() != _bestList.size())
		INFO("Best Peer - Peers connected : ", _mapPeers.size(), "/", _mapGroupAddress.size(), " ; target count : ", _bestList.size(), " ; GroupMedia count : ", _mapGroupMedias.size())
}

void NetGroup::sendGroupReport(P2PSession* pPeer, bool initiator) {
	TRACE("Preparing the Group Report message (type 0A) for peer ", pPeer->peerId)

	auto itNode = _mapHeardList.find(pPeer->peerId);
	if (itNode == _mapHeardList.end()) {
		ERROR("Unable to find the peer ", pPeer->peerId, " in the Heard list") // implementation error
		return;
	}

	set<string> bestList;
	buildBestList(itNode->second.groupAddress, bestList);

	// Calculate the total size to allocate sufficient memory
	UInt32 sizeTotal = (UInt32)(pPeer->peerAddress().host().size() + _conn.serverAddress().host().size() + 12);
	Int64 timeNow(Time::Now());
	for (auto it1 : bestList) {
		itNode = _mapHeardList.find(it1);
		if (itNode != _mapHeardList.end())
			sizeTotal += itNode->second.addressesSize() + PEER_ID_SIZE + 5 + ((itNode->second.lastGroupReport > 0) ? Util::Get7BitValueSize((timeNow - itNode->second.lastGroupReport) / 1000) : 1);
	}
	_reportBuffer.resize(sizeTotal);

	BinaryWriter writer(BIN _reportBuffer.data(), _reportBuffer.size());
	writer.write8(0x0A);
	writer.write8(pPeer->peerAddress().host().size() + 4);
	writer.write8(0x0D);
	RTMFP::WriteAddress(writer, pPeer->peerAddress(), RTMFP::ADDRESS_PUBLIC);
	writer.write8(_conn.serverAddress().host().size() + 4);
	writer.write8(0x0A);
	RTMFP::WriteAddress(writer, _conn.serverAddress(), RTMFP::ADDRESS_REDIRECTION);
	writer.write8(0);

	for (auto it2 : bestList) {
		itNode = _mapHeardList.find(it2);
		if (itNode != _mapHeardList.end()) {

			UInt64 timeElapsed = (UInt64)((itNode->second.lastGroupReport > 0) ? ((timeNow - itNode->second.lastGroupReport) / 1000) : 0);
			TRACE("Group 0A argument - Peer ", itNode->first, " - elapsed : ", timeElapsed) //, " (latency : ", itPeer.second->latency(), ")")
			writer.write8(0x22).write(itNode->second.rawId.data(), PEER_ID_SIZE+2);
			writer.write7BitLongValue(timeElapsed);
			writer.write8(itNode->second.addressesSize());
			writer.write8(0x0A);
			RTMFP::WriteAddress(writer, itNode->second.hostAddress, RTMFP::ADDRESS_REDIRECTION);
			for (auto itAddress : itNode->second.addresses)
				if (itAddress.second != RTMFP::ADDRESS_LOCAL)
					RTMFP::WriteAddress(writer, itAddress.first, itAddress.second);
			writer.write8(0);
		}
	}

	TRACE("Sending the group report to ", pPeer->peerId)
	pPeer->groupReportInitiator = initiator;
	pPeer->sendGroupReport(_reportBuffer.data(), _reportBuffer.size());
}

void NetGroup::manageBestConnections() {

	// Close old peers
	auto it2Close = _mapPeers.begin();
	while (it2Close != _mapPeers.end()) {
		if (_bestList.find(it2Close->first) == _bestList.end())
			it2Close->second->askPeer2Disconnect();
		++it2Close;
	}

	// Connect to new peers
	for (auto it : _bestList) {
		if (_mapPeers.find(it) == _mapPeers.end()) {
			auto itNode = _mapHeardList.find(it);
			if (itNode == _mapHeardList.end())
				WARN("Unable to find the peer ", it) // implementation error, should not happen
			else {
				DEBUG("Best Peer - Connecting to peer ", it, "...")
				_conn.connect2Peer(it.c_str(), stream.c_str(), itNode->second.addresses, itNode->second.hostAddress);
			}
		}
	}
}

unsigned int NetGroup::callFunction(const char* function, int nbArgs, const char** args) {

	for (auto& itGroupMedia : _mapGroupMedias)
		itGroupMedia.second.callFunction(function, nbArgs, args);

	return 1;
}

void NetGroup::ReadGroupConfig(shared_ptr<RTMFPGroupConfig>& parameters, PacketReader& packet) {

	// Update the NetGroup stream properties
	UInt8 size = 0, id = 0;
	unsigned int value = 0;
	parameters->availabilitySendToAll = 0;
	while (packet.available()) {
		if ((size = packet.read8()) == 0)
			continue;
		id = packet.read8();
		value = (size > 1) ? (unsigned int)packet.read7BitLongValue() : 0;
		switch (id) {
		case NetGroup::UNKNWON_PARAMETER:
			break;
		case NetGroup::WINDOW_DURATION:
			parameters->windowDuration = value;
			TRACE("Window Duration : ", parameters->windowDuration, "ms");
			break;
		case NetGroup::OBJECT_ENCODING:
			if (value != 300000)
				ERROR("Unexpected object encoding value : ", value) // TODO: not sure it is object encoding!
				break;
		case NetGroup::UPDATE_PERIOD:
			if (value != parameters->availabilityUpdatePeriod) {
				parameters->availabilityUpdatePeriod = value;
				TRACE("Avaibility Update period : ", parameters->availabilityUpdatePeriod, "ms");
			}
			break;
		case NetGroup::SEND_TO_ALL:
			parameters->availabilitySendToAll = 1;
			TRACE("Availability Send to All ON");
			return;
		case NetGroup::FETCH_PERIOD:
			parameters->fetchPeriod = value;
			TRACE("Fetch period : ", parameters->fetchPeriod, "ms"); break;
			break;
		}
	}
}

bool NetGroup::readGroupReport(PacketReader& packet) {
	string tmp, newPeerId, rawId;
	SocketAddress myAddress, serverAddress;
	UInt8 addressType;
	PEER_LIST_ADDRESS_TYPE listAddresses;
	SocketAddress hostAddress(_conn.serverAddress());

	UInt8 size = packet.read8();
	while (size == 1) { // TODO: check what this means
		packet.next();
		size = packet.read8();
	}

	// Read my address & the far peer addresses
	UInt8 tmpMarker = packet.read8();
	if (tmpMarker != 0x0D) {
		ERROR("Unexpected marker : ", Format<UInt8>("%.2x", tmpMarker), " - Expected 0D")
		return false;
	}
	RTMFP::ReadAddress(packet, myAddress, addressType);
	TRACE("Group Report - My address : ", myAddress.toString())
	
	size = packet.read8();
	tmpMarker = packet.read8();
	if (tmpMarker != 0x0A) {
		ERROR("Unexpected marker : ", Format<UInt8>("%.2x", tmpMarker), " - Expected 0A")
		return false;
	}
	BinaryReader peerAddressReader(packet.current(), size - 1);
	RTMFP::ReadAddresses(peerAddressReader, listAddresses, hostAddress);
	packet.next(size - 1);

	// Loop on each peer of the NetGroup
	bool newPeers = false;
	while (packet.available() > 4) {
		if ((tmpMarker = packet.read8()) != 00) {
			ERROR("Unexpected marker : ", Format<UInt8>("%.2x", tmpMarker), " - Expected 00")
			break;
		}
		size = packet.read8();
		if (size == 0x22) {
			packet.read(size, rawId);
			Util::FormatHex(BIN rawId.data() + 2, PEER_ID_SIZE, newPeerId);
			if (String::ICompare(rawId, "\x21\x0F", 2) != 0) {
				ERROR("Unexpected parameter : ", newPeerId, " - Expected Peer Id")
				break;
			}
			TRACE("Group Report - Peer ID : ", newPeerId)
		}
		else if (size > 7)
			packet.next(size); // ignore the addresses if peerId not set
		else
			TRACE("Empty parameter...")

		UInt64 time = packet.read7BitLongValue();
		TRACE("Group Report - Time elapsed : ", time)
		size = packet.read8(); // Addresses size

		// New peer, read its addresses
		if (size >= 0x08 && newPeerId != _conn.peerId() && _mapHeardList.find(newPeerId) == _mapHeardList.end() && *packet.current() == 0x0A) {

			BinaryReader addressReader(packet.current() + 1, size - 1); // +1 to ignore 0A
			hostAddress = _conn.serverAddress(); // default host is the same as ours
			listAddresses.clear();
			if (RTMFP::ReadAddresses(addressReader, listAddresses, hostAddress)) {
				newPeers = true;
				addPeer2HeardList(newPeerId.c_str(), rawId.data(), listAddresses, hostAddress, time);  // To avoid memory sharing we use c_str() (copy-on-write implementation on linux)
			}
		}
		packet.next(size);
	}

	return newPeers;
}