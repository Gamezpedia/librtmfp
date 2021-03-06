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

#pragma once

#include "FlowManager.h"
#include "Mona/StopWatch.h"
#include "PeerMedia.h"

class RTMFPSession;
struct RTMFPGroupConfig;
class P2PSession;

// Peer Group Events
namespace P2PEvents {
	struct OnPeerGroupReport : Mona::Event<void(P2PSession*, Mona::PacketReader&, bool)> {}; // called when receiving a Group Report message from the peer
	struct OnNewMedia : Mona::Event<bool(const std::string&, std::shared_ptr<PeerMedia>&, const std::string&, const std::string&, Mona::PacketReader&)> {}; // called when a new PeerMedia is called (new stream available for the peer)
	struct OnPeerGroupBegin : Mona::Event<void(P2PSession*)> {}; // called when receiving a Group Begin message from the peer
	struct OnPeerClose : Mona::Event<void(const std::string&)> {}; // called when the peer is closing
	struct OnPeerGroupAskClose : Mona::Event<bool(const std::string&)> {}; // called when a peer ask to close its session
}

/**************************************************
P2PSession represents a direct P2P connection 
with another peer
*/
class P2PSession : public FlowManager,
	public P2PEvents::OnPeerGroupBegin,
	public P2PEvents::OnPeerGroupReport,
	public P2PEvents::OnNewMedia,
	public P2PEvents::OnPeerClose,
	public P2PEvents::OnPeerGroupAskClose {
public:
	P2PSession(RTMFPSession* parent, std::string id, Invoker* invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent, /*const PEER_LIST_ADDRESS_TYPE& addresses,*/
		const Mona::SocketAddress& host, bool responder, bool group);

	virtual ~P2PSession();

	// Add a command to the main stream (play/publish/netgroup)
	virtual void					addCommand(CommandType command, const char* streamName, bool audioReliable = false, bool videoReliable = false);

	// Set the tag used for this connection (responder mode)
	void							setTag(const std::string& tag) { _tag = tag; }

	// Call a function on the peer side
	// return 0 if it fails, 1 otherwise
	unsigned int					callFunction(const char* function, int nbArgs, const char** args);

	const Mona::SocketAddress&		peerAddress() { FATAL_ASSERT(_pConnection) return _pConnection->address(); }

	// Create a flow for special signatures (NetGroup)
	virtual RTMFPFlow*				createSpecialFlow(Mona::Exception& ex, Mona::UInt64 id, const std::string& signature, Mona::UInt64 idWriterRef);

	// Close the group writers but keep the connection open if full is false
	virtual void					close(bool abrupt=false);

	// Close the Group connection to peer
	void							closeGroup(bool abrupt);

	// Return the name of the session
	virtual const std::string&		name() { return peerId; }

	// Return the raw peerId of the session (for RTMFPConnection)
	virtual const std::string&		epd() { return rawId; }

	// Return the known addresses of the peer (for RTMFPSession)
	const PEER_LIST_ADDRESS_TYPE&	addresses() { return _knownAddresses; }

	// Subscribe to all events of the connection and add it to the list of known addresses
	virtual void					subscribe(std::shared_ptr<RTMFPConnection>& pConnection);

	// Called by parent when we are connected
	void							onConnection(std::shared_ptr<RTMFPConnection>& pConnection);

	/*** NetGroup functions ***/

	// Write the Group publication infos
	std::shared_ptr<PeerMedia>& getPeerMedia(const std::string& streamKey);

	// Send the group report (message 0A)
	void sendGroupReport(const Mona::UInt8* data, Mona::UInt32 size);

	// Send the group begin message (02 + 0E messages), return true if the message has been sent
	bool sendGroupBegin();

	// Send the Group Peer Connect request
	void sendGroupPeerConnect();

	// called by a PeerMedia to create the media writer
	bool createMediaWriter(std::shared_ptr<RTMFPWriter>& pWriter, Mona::UInt64 flowIdRef);

	// called by PeerMedia to close the media report and the media flows
	void closeFlow(Mona::UInt64 id);

	// Ask a peer from the group to disconnect
	void askPeer2Disconnect();

	// Manage the flows
	virtual void				manage() { FlowManager::manage(); }

	/*** Public members ***/

	std::string						rawId; // Peer Id in binary format + header (210f)
	std::string						peerId; // Peer Id of the peer connected
	Mona::SocketAddress				hostAddress; // Host address (server address)

	// NetGroup members
	bool							groupFirstReportSent; // True if the first group report has been sent
	bool							groupReportInitiator; // True if we are the initiator of last Group Report (to avoid endless exchanges)

protected:
	// Handle play request (only for P2PSession)
	virtual bool					handlePlay(const std::string& streamName, Mona::UInt16 streamId, Mona::UInt64 flowId, double cbHandler);

	// Handle a Writer close message (type 5E)
	virtual void					handleWriterException(std::shared_ptr<RTMFPWriter>& pWriter);

	// Handle a P2P address exchange message (Only for RTMFPConnection)
	virtual void					handleP2PAddressExchange(Mona::PacketReader& reader);
	
	// Handle a new writer creation
	virtual void					handleNewWriter(std::shared_ptr<RTMFPWriter>& pWriter);

	// Handle data available or not event (asynchronous read only)
	virtual void					handleDataAvailable(bool isAvailable);

private:

	// Handle a NetGroup connection message from a peer connected (only for P2PSession)
	void							handleGroupHandshake(const std::string& groupId, const std::string& key, const std::string& id);

	static Mona::UInt32										P2PSessionCounter; // Global counter for generating incremental P2P sessions id
	RTMFPSession*											_parent; // RTMFPConnection related to
	PEER_LIST_ADDRESS_TYPE									_knownAddresses; // list of known addresses of the peer/server
	std::string												_streamName; // playing stream name
	bool													_responder; // is responder?

	// Group members
	std::shared_ptr<Mona::Buffer>							_groupConnectKey; // Encrypted key used to connect to the peer
	bool													_groupConnectSent; // True if group connection request has been sent to peer
	bool													_groupBeginSent; // True if the group messages 02 + 0E have been sent
	bool													_isGroup; // True if this peer connection it part of a NetGroup
	Mona::Time												_lastTryDisconnect; // Last time we ask peer to disconnect

	std::shared_ptr<RTMFPWriter>							_pReportWriter; // Writer for report messages
	std::shared_ptr<RTMFPWriter>							_pNetStreamWriter; // Writer for NetStream P2P direct messages
	std::shared_ptr<RTMFPWriter>							_pLastWriter; // Last created writer

	std::map<Mona::UInt64, std::shared_ptr<PeerMedia>>		_mapWriter2PeerMedia; // map of writer id to peer media
	std::map<std::string, std::shared_ptr<PeerMedia>>		_mapStream2PeerMedia; // map of stream key to peer media
	std::map<Mona::UInt64, std::shared_ptr<PeerMedia>>		_mapFlow2PeerMedia; // map of flow id to peer media

	FlashConnection::OnGroupHandshake::Type					onGroupHandshake;
	FlashConnection::OnGroupMedia::Type						onGroupMedia;
	FlashConnection::OnGroupReport::Type					onGroupReport;
	FlashConnection::OnGroupPlayPush::Type					onGroupPlayPush;
	FlashConnection::OnGroupPlayPull::Type					onGroupPlayPull;
	FlashConnection::OnFragmentsMap::Type					onFragmentsMap;
	FlashConnection::OnGroupBegin::Type						onGroupBegin;
	FlashConnection::OnFragment::Type						onFragment;
	FlashConnection::OnGroupAskClose::Type					onGroupAskClose;
};
