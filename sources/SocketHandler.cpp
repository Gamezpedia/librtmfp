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

#include "SocketHandler.h"
#include "Invoker.h"
#include "RTMFPSession.h"
#include "Mona/Logs.h"

using namespace Mona;
using namespace std;

SocketHandler::SocketHandler(Invoker* invoker, RTMFPSession* pSession) : _pInvoker(invoker), _acceptAll(false), _pMainSession(pSession) {
	onPacket = [this](PoolBuffer& pBuffer, const SocketAddress& address) {
		if (_pMainSession->status < RTMFP::NEAR_CLOSED) {

			lock_guard<mutex> lock(_mutexConnections);
			auto itConnection = _mapAddress2Connection.find(address);
			if (itConnection != _mapAddress2Connection.end())
				itConnection->second->process(pBuffer);
			else {
				DEBUG("Input packet from a new address : ", address.toString());
				_pDefaultConnection->setAddress(address);
				_pDefaultConnection->process(pBuffer);
			}
		}
	};
	onError = [this](const Exception& ex) {
		SocketAddress address;
		DEBUG("Socket error : ", ex.error(), " from ", _pSocket->peerAddress(address).toString())
	};

	_pSocket.reset(new UDPSocket(_pInvoker->sockets));
	_pSocket->OnError::subscribe(onError);
	_pSocket->OnPacket::subscribe(onPacket);
	_pSocketIPV6.reset(new UDPSocket(_pInvoker->sockets));
	_pSocketIPV6->OnError::subscribe(onError);
	_pSocketIPV6->OnPacket::subscribe(onPacket);
	Exception ex;
	SocketAddress address(SocketAddress::Wildcard(IPAddress::IPv6));
	if (!_pSocketIPV6->bind(ex, address))
		WARN("Unable to bind [::], ipv6 will not work : ", ex.error())

	_pDefaultConnection.reset(new DefaultConnection(this));
}

SocketHandler::~SocketHandler() {
	close();
}

void SocketHandler::close() {
	lock_guard<mutex> lock(_mutexConnections);
	for (auto itConnection = _mapAddress2Connection.begin(); itConnection != _mapAddress2Connection.end(); itConnection++)
		deleteConnection(itConnection);
	_mapAddress2Connection.clear();

	// Unsubscribing to socket : we don't want to receive packets anymore
	if (_pSocket) {
		_pSocket->OnPacket::unsubscribe(onPacket);
		_pSocket->OnError::unsubscribe(onError);
		_pSocket->close();
	}
	if (_pSocketIPV6) {
		_pSocketIPV6->OnPacket::unsubscribe(onPacket);
		_pSocketIPV6->OnError::unsubscribe(onError);
		_pSocketIPV6->close();
	}
}

const PoolBuffers& SocketHandler::poolBuffers() {
	return _pInvoker->poolBuffers;
}

const string& SocketHandler::peerId() { 
	return _pMainSession->peerId();
}

bool SocketHandler::diffieHellman(DiffieHellman * &pDh) {
	if (!_diffieHellman.initialized()) {
		Exception ex;
		if (!_diffieHellman.initialize(ex)) {
			ERROR("Unable to initialize diffie hellman object : ", ex.error())
			return false;
		}
	}
	pDh = &_diffieHellman;
	return true;
}

void SocketHandler::addP2PConnection(const string& rawId, const string& peerId, const string& tag, const SocketAddress& hostAddress) {

	//lock_guard<mutex> lock(_mutexConnections);
	_mapTag2Peer.emplace(piecewise_construct, forward_as_tuple(tag), forward_as_tuple(rawId, peerId, hostAddress));
}

bool SocketHandler::onNewPeerId(const string& rawId, const string& peerId, const SocketAddress& address) {
	//lock_guard<mutex> lock(_mutexConnections);
	auto itConnection = _mapAddress2Connection.find(address);
	return OnNewPeerId::raise<false>(itConnection->second, rawId, peerId);
}


void SocketHandler::onConnection(const SocketAddress& address, const string& name) {
	//lock_guard<mutex> lock(_mutexConnections);
	auto itConnection = _mapAddress2Connection.find(address);
	OnConnection::raise(itConnection->second, name);
}

bool  SocketHandler::addConnection(shared_ptr<RTMFPConnection>& pConn, const SocketAddress& address, FlowManager* session, bool responder, bool p2p) {
	//lock_guard<mutex> lock(_mutexConnections);
	auto itConnection = _mapAddress2Connection.lower_bound(address);
	if (itConnection == _mapAddress2Connection.end() || itConnection->first != address) {
		pConn = _mapAddress2Connection.emplace_hint(itConnection, piecewise_construct, forward_as_tuple(address), forward_as_tuple(new RTMFPConnection(address, this, session, responder, p2p)))->second;
		pConn->OnIdBuilt::subscribe((OnIdBuilt&)*this);
		if (session)
			session->subscribe(pConn);
		return true;
	}
	DEBUG("Connection already exists at address ", address.toString(), ", nothing done")
	pConn = itConnection->second;
	return false;
}

void SocketHandler::deleteConnection(const MAP_ADDRESS2CONNECTION::iterator& itConnection) {
	TRACE("Closing connection to ", itConnection->first.toString())
	itConnection->second->close();
	itConnection->second->OnIdBuilt::unsubscribe((OnIdBuilt&)*this);
}

void SocketHandler::manage() {
	lock_guard<mutex> lock(_mutexConnections);

	if (!_mapTag2Peer.empty()) {

		// Ask server to send p2p addresses
		auto itPeer = _mapTag2Peer.begin();
		while (itPeer != _mapTag2Peer.end()) {
			WaitingPeer& peer = itPeer->second;
			if (!peer.attempt || peer.lastAttempt.isElapsed(peer.attempt * 1500)) {
				if (peer.attempt++ == 11) {
					DEBUG("Connection to ", peer.peerId, " has reached 11 attempt without answer, removing the peer...")
					_mapTag2Peer.erase(itPeer++);
					continue;
				}

				DEBUG("Sending new P2P handshake 30 to server (peerId : ", peer.peerId, "; ", peer.attempt, "/11)")
				_pDefaultConnection->setAddress(peer.hostAddress);
				_pDefaultConnection->sendHandshake30(peer.rawId, itPeer->first);
				peer.lastAttempt.update();
			}
			++itPeer;
		}
	}

	// Manage all connections
	for (auto itConnection : _mapAddress2Connection)
		itConnection.second->manage();

	// Delete old connections
	auto itConnection2 = _mapAddress2Connection.begin();
	while (itConnection2 != _mapAddress2Connection.end()) {
		if (itConnection2->second->failed()) {
			deleteConnection(itConnection2);
			_mapAddress2Connection.erase(itConnection2++);
		} else
			++itConnection2;
	}

	_pDefaultConnection->manage();
}

void SocketHandler::onP2PAddresses(const string& tagReceived, BinaryReader& reader) {
	//lock_guard<mutex> lock(_mutexConnections);
	auto it = _mapTag2Peer.find(tagReceived);
	if (it == _mapTag2Peer.end()) {
		DEBUG("Handshake 71 received but no p2p connection found with tag (possible old request)")
		return;
	}

	// Read addresses
	SocketAddress hostAddress;
	PEER_LIST_ADDRESS_TYPE addresses;
	RTMFP::ReadAddresses(reader, addresses, hostAddress);

	// Update addresses and send handshake 30 to far server if no handshake 70 received
	if (OnP2PAddresses::raise<false>(it->second.peerId, addresses) && hostAddress && it->second.hostAddress != hostAddress) {

		// Send handshake 30 to far server
		DEBUG("Sending P2P handshake 30 to far server at ", hostAddress.toString()," (peerId : ", it->second.peerId, ")")
		it->second.hostAddress = hostAddress; // update host address with far server address
		_pDefaultConnection->setAddress(hostAddress);
		_pDefaultConnection->sendHandshake30(it->second.rawId, it->first);
		++it->second.attempt;
		it->second.lastAttempt.update();
	}
}


void SocketHandler::onPeerHandshake30(const string& id, const string& tag, const SocketAddress& address) {

	if (id != peerId()) {
		ERROR("Unexpected peer ID in handshake 30 : ", id, ", connection rejected")
		return;
	}

	auto itPeer = _mapTag2Peer.find(tag);
	if (itPeer == _mapTag2Peer.end())
		OnPeerHandshake30::raise(tag, address);
	else
		DEBUG("Handshake 30 received but the connection exists")
}

bool SocketHandler::onPeerHandshake70(const string& tagReceived, const string& farkey, const string& cookie, const SocketAddress& address, bool createConnection, bool isP2P) {

	if (!isP2P)
		return OnPeerHandshake70::raise<false>("", address, farkey, cookie, false);

	auto itPeer = _mapTag2Peer.find(tagReceived);
	if (itPeer != _mapTag2Peer.end()) {
		bool res = OnPeerHandshake70::raise<false>(itPeer->second.peerId, address, farkey, cookie, createConnection); // (If it is an unknown address, we create the connection)
		_mapTag2Peer.erase(itPeer);
		return res;
	}
	
	TRACE("Unknown tag received with handshake 70 from address ", address.toString(), " (possible old connection)")
	return false;
}