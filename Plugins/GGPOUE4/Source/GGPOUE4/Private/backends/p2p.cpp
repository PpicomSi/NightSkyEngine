/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#include "p2p.h"
#include "GGPOUE4.h"

#ifdef _MSC_VER
#pragma warning(disable:5038)
#endif
static const int RECOMMENDATION_INTERVAL           = 100;
static const int DEFAULT_DISCONNECT_TIMEOUT        = 15000;
static const int DEFAULT_DISCONNECT_NOTIFY_START   = 2000;

Peer2PeerBackend::Peer2PeerBackend(GGPOSessionCallbacks *cb,
                                   const char *gamename,
                                   ConnectionManager* connection_manager,
                                   int num_players,
                                   int input_size) :
    _num_players(num_players),
    _input_size(input_size),
    _sync(_local_connect_status),
    _disconnect_timeout(DEFAULT_DISCONNECT_TIMEOUT),
    _disconnect_notify_start(DEFAULT_DISCONNECT_NOTIFY_START),
    _num_spectators(0),
    _next_spectator_frame(0)
{
   _callbacks = *cb;
   _synchronizing = true;
   _next_recommended_sleep = 0;
   _connection_manager = connection_manager;
   /*
    * Initialize the synchronziation layer
    */
   Sync::Config config = {};
   config.num_players = num_players;
   config.input_size = input_size;
   config.callbacks = _callbacks;
   config.num_prediction_frames = MAX_PREDICTION_FRAMES;
   _sync.Init(config);

   /*
    * Initialize the UDP container
    */
   _udp.Init(&_poll, this, connection_manager);

   _endpoints = new UdpProtocol[_num_players];
   memset(_local_connect_status, 0, sizeof(_local_connect_status));
   for (int i = 0; i < ARRAY_SIZE(_local_connect_status); i++) {
      _local_connect_status[i].last_frame = -1;
   }

   /*
    * Preload the ROM
    */
   _callbacks.begin_game(gamename);
}
  
Peer2PeerBackend::~Peer2PeerBackend()
{
   delete [] _endpoints;
}

void
Peer2PeerBackend::AddRemotePlayer(int connection_id,
                                  int queue)
{
   /*
    * Start the state machine (xxx: no)
    */
   _synchronizing = true;
   remoteplayerId    =connection_id;
   remoteplayerQueueu=queue;
   _endpoints[queue].Init(&_udp, _poll, queue, connection_id, _local_connect_status);
   _endpoints[queue].SetDisconnectTimeout(_disconnect_timeout);
   _endpoints[queue].SetDisconnectNotifyStart(_disconnect_notify_start);
   _endpoints[queue].Synchronize();
}

GGPOErrorCode Peer2PeerBackend::AddSpectator(int connection_id)
{
   if (_num_spectators == GGPO_MAX_SPECTATORS) {
      return GGPO_ERRORCODE_TOO_MANY_SPECTATORS;
   }
   /*
    * Currently, we can only add spectators before the game starts.
    */
   if (!_synchronizing) {
      return GGPO_ERRORCODE_INVALID_REQUEST;
   }
   int queue = _num_spectators++;

   _spectators[queue].Init(&_udp, _poll, queue + 1000, connection_id, _local_connect_status);
   _spectators[queue].SetDisconnectTimeout(_disconnect_timeout);
   _spectators[queue].SetDisconnectNotifyStart(_disconnect_notify_start);
   _spectators[queue].Synchronize();

   return GGPO_OK;
}

GGPOErrorCode
Peer2PeerBackend::DoPoll(int timeout)
{
   if (!_sync.InRollback()) {
      _poll.Pump(0);

      PollUdpProtocolEvents();

      if (!_synchronizing) {
         _sync.CheckSimulation(timeout);

         // notify all of our endpoints of their local frame number for their
         // next connection quality report
         int current_frame = _sync.GetFrameCount();
         for (int i = 0; i < _num_players; i++) {
            _endpoints[i].SetLocalFrameNumber(current_frame);
         }

         int total_min_confirmed;
         if (_num_players <= 2) {
            total_min_confirmed = Poll2Players(current_frame);
         } else {
            total_min_confirmed = PollNPlayers(current_frame);
         }

		 UE_LOG(GGPOLOG, Verbose, TEXT("Peer2PeerBackend::DoPoll last confirmed frame in p2p backend is %d."),
			 total_min_confirmed);
         if (total_min_confirmed >= 0) {
			check(total_min_confirmed != INT_MAX);
            if (_num_spectators > 0) {
               while (_next_spectator_frame <= total_min_confirmed) {
				   UE_LOG(GGPOLOG, Verbose, TEXT("Peer2PeerBackend::DoPoll pushing frame %d to spectators."),
					   _next_spectator_frame);
   
                  GameInput input;
                  input.frame = _next_spectator_frame;
                  input.size = _input_size * _num_players;
                  _sync.GetConfirmedInputs(input.bits, _input_size * _num_players, _next_spectator_frame);
                  for (int i = 0; i < _num_spectators; i++) {
                     _spectators[i].SendInput(input);
                  }
                  _next_spectator_frame++;
               }
            }

			UE_LOG(GGPOLOG, Verbose, TEXT("Peer2PeerBackend::DoPoll setting confirmed frame in sync to %d."),
				total_min_confirmed);
            _sync.SetLastConfirmedFrame(total_min_confirmed);
         }

         // send timesync notifications if now is the proper time
         if (current_frame > _next_recommended_sleep) {
            int interval = 0;
            for (int i = 0; i < _num_players; i++) {
               auto t = _endpoints[i].RecommendFrameDelay();
               if(t<0)
               {
                  GGPOEvent info;
                  info.code = GGPO_EVENTCODE_TIMESYNC_BEHIND;
                  info.u.timesync.frames_ahead = t*-1;
                  _callbacks.on_event(&info);
                  interval=0;
                  break;
               }
               interval = MAX(interval,t);
            }

            if (interval > 0) {
               GGPOEvent info;
               info.code = GGPO_EVENTCODE_TIMESYNC;
               info.u.timesync.frames_ahead = interval;
               _callbacks.on_event(&info);
               _next_recommended_sleep = current_frame + RECOMMENDATION_INTERVAL;
            }
         }
         // XXX: this is obviously a farce...
         if (timeout) {
			 PlatformGGPO::SleepForMilliseconds(1);
         }
      }
   }
   return GGPO_OK;
}

int Peer2PeerBackend::Poll2Players(int current_frame)
{
   int i;

   // discard confirmed frames as appropriate
   int total_min_confirmed = MAX_INT;
   for (i = 0; i < _num_players; i++) {
      bool queue_connected = true;
      if (_endpoints[i].IsRunning()) {
         int ignore;
         queue_connected = _endpoints[i].GetPeerConnectStatus(i, &ignore);
      }
      if (!_local_connect_status[i].disconnected) {
         total_min_confirmed = MIN(_local_connect_status[i].last_frame, total_min_confirmed);
      }

	  UE_LOG(GGPOLOG, Verbose, TEXT("Peer2PeerBackend::Poll2Players  local endp: connected = %d, last_received = %d, total_min_confirmed = %d."), !_local_connect_status[i].disconnected, _local_connect_status[i].last_frame, total_min_confirmed);
	  
	  if (!queue_connected && !_local_connect_status[i].disconnected) {
		 UE_LOG(GGPOLOG, Verbose, TEXT("Peer2PeerBackend::Poll2Players disconnecting i %d by remote request."), i);
         DisconnectPlayerQueue(i, total_min_confirmed);
      } 
	  UE_LOG(GGPOLOG, Verbose, TEXT("Peer2PeerBackend::Poll2Players total_min_confirmed = %d."), total_min_confirmed);
   }
   return total_min_confirmed;
}

int Peer2PeerBackend::PollNPlayers(int current_frame)
{
   int i, queue, last_received;

   // discard confirmed frames as appropriate
   int total_min_confirmed = MAX_INT;
   for (queue = 0; queue < _num_players; queue++) {
      bool queue_connected = true;
      int queue_min_confirmed = MAX_INT;
	  UE_LOG(GGPOLOG, Verbose, TEXT("Peer2PeerBackend::PollNPlayers considering queue %d."), queue);
      for (i = 0; i < _num_players; i++) {
         // we're going to do a lot of logic here in consideration of endpoint i.
         // keep accumulating the minimum confirmed point for all n*n packets and
         // throw away the rest.
         if (_endpoints[i].IsRunning()) {
            bool connected = _endpoints[i].GetPeerConnectStatus(queue, &last_received);

            queue_connected = queue_connected && connected;
            queue_min_confirmed = MIN(last_received, queue_min_confirmed);
			UE_LOG(GGPOLOG, Verbose, TEXT("Peer2PeerBackend::PollNPlayers endpoint %d: connected = %d, last_received = %d, queue_min_confirmed = %d."), i, connected, last_received, queue_min_confirmed);
         } else {
			UE_LOG(GGPOLOG, Verbose, TEXT("Peer2PeerBackend::PollNPlayers endpoint %d: ignoring... not running."), i);
         }
      }
      // merge in our local status only if we're still connected!
      if (!_local_connect_status[queue].disconnected) {
         queue_min_confirmed = MIN(_local_connect_status[queue].last_frame, queue_min_confirmed);
      }
	  UE_LOG(GGPOLOG, Verbose, TEXT("Peer2PeerBackend::PollNPlayers  local endp: connected = %d, last_received = %d, queue_min_confirmed = %d."), !_local_connect_status[queue].disconnected, _local_connect_status[queue].last_frame, queue_min_confirmed);

      if (queue_connected) {
         total_min_confirmed = MIN(queue_min_confirmed, total_min_confirmed);
      } else {
         // check to see if this disconnect notification is further back than we've been before.  If
         // so, we need to re-adjust.  This can happen when we detect our own disconnect at frame n
         // and later receive a disconnect notification for frame n-1.
         if (!_local_connect_status[queue].disconnected || _local_connect_status[queue].last_frame > queue_min_confirmed) {
			 UE_LOG(GGPOLOG, Verbose, TEXT("Peer2PeerBackend::PollNPlayers disconnecting queue %d by remote request."), queue);
            DisconnectPlayerQueue(queue, queue_min_confirmed);
         }
      }
	  UE_LOG(GGPOLOG, Verbose, TEXT("Peer2PeerBackend::PollNPlayers  total_min_confirmed = %d."), total_min_confirmed);
   }
   return total_min_confirmed;
}


GGPOErrorCode
Peer2PeerBackend::AddPlayer(GGPOPlayer *player,
                            GGPOPlayerHandle *handle)
{
   if (player->type == GGPOPlayerType::GGPO_PLAYERTYPE_SPECTATOR) {
      return AddSpectator(player->connection_id);
   }

   int queue = player->player_num - 1;
   if (player->player_num < 1 || player->player_num > _num_players) {
      return GGPO_ERRORCODE_PLAYER_OUT_OF_RANGE;
   }
   *handle = QueueToPlayerHandle(queue);


   if (player->type == GGPOPlayerType::GGPO_PLAYERTYPE_REMOTE) {
      AddRemotePlayer(player->connection_id, queue);
   }
   return GGPO_OK;
}

GGPOErrorCode
Peer2PeerBackend::AddLocalInput(GGPOPlayerHandle player,
                                void *values,
                                int size)
{
   int queue;
   GameInput input;
   GGPOErrorCode result;

   if (_sync.InRollback()) {
      return GGPO_ERRORCODE_IN_ROLLBACK;
   }
   if (_synchronizing) {
      return GGPO_ERRORCODE_NOT_SYNCHRONIZED;
   }
   
   result = PlayerHandleToQueue(player, &queue);
   if (!GGPO_SUCCEEDED(result)) {
      return result;
   }

   input.init(-1, (char *)values, size);

   // Feed the input for the current frame into the synchronzation layer.
   if (!_sync.AddLocalInput(queue, input)) {
      return GGPO_ERRORCODE_PREDICTION_THRESHOLD;
   }

   if (input.frame != GameInput::NullFrame) { // xxx: <- comment why this is the case
      // Update the local connect status state to indicate that we've got a
      // confirmed local frame for this player.  this must come first so it
      // gets incorporated into the next packet we send.

	  UE_LOG(GGPOLOG, Verbose, TEXT("Peer2PeerBackend::AddLocalInput setting local connect status for local queue %d to %d"), queue, input.frame);
	  _local_connect_status[queue].last_frame = input.frame;

      // Send the input to all the remote players.
      for (int i = 0; i < _num_players; i++) {
         if (_endpoints[i].IsInitialized()) {
            _endpoints[i].SendInput(input);
         }
      }
   }

   return GGPO_OK;
}


GGPOErrorCode
Peer2PeerBackend::SyncInput(void *values,
                            int size,
                            int *disconnect_flags)
{
   int flags;

   // Wait until we've started to return inputs.
   if (_synchronizing) {
      return GGPO_ERRORCODE_NOT_SYNCHRONIZED;
   }
   flags = _sync.SynchronizeInputs(values, size);
   if (disconnect_flags) {
      *disconnect_flags = flags;
   }
   return GGPO_OK;
}

GGPOErrorCode
Peer2PeerBackend::IncrementFrame(void)
{  
   UE_LOG(GGPOLOG, Verbose, TEXT("Peer2PeerBackend::IncrementFrame end of frame %d"), _sync.GetFrameCount());
   _sync.IncrementFrame();
   DoPoll(0);
   PollSyncEvents();

   return GGPO_OK;
}


void
Peer2PeerBackend::PollSyncEvents(void)
{
   Sync::Event e;
   while (_sync.GetEvent(e)) {
      OnSyncEvent(e);
   }
   return;
}

void
Peer2PeerBackend::PollUdpProtocolEvents(void)
{
   UdpProtocol::Event evt;
   for (int i = 0; i < _num_players; i++) {
      while (_endpoints[i].GetEvent(evt)) {
         OnUdpProtocolPeerEvent(evt, i);
      }
   }
   for (int i = 0; i < _num_spectators; i++) {
      while (_spectators[i].GetEvent(evt)) {
         OnUdpProtocolSpectatorEvent(evt, i);
      }
   }
}

void
Peer2PeerBackend::OnUdpProtocolPeerEvent(UdpProtocol::Event &evt, int queue)
{
   OnUdpProtocolEvent(evt, QueueToPlayerHandle(queue));
   switch (evt.type) {
      case UdpProtocol::Event::Input:
         if (!_local_connect_status[queue].disconnected) {
            int current_remote_frame = _local_connect_status[queue].last_frame;
            int new_remote_frame = evt.u.input.input.frame;

			UE_LOG(GGPOLOG, Verbose, TEXT("Peer2PeerBackend::OnUdpProtocolPeerEvent Current remote frame %d new remote frame %d"),current_remote_frame, new_remote_frame);
            if(current_remote_frame == -1 || new_remote_frame == (current_remote_frame + 1))
            {
               printf("Peer2PeerBackend::OnUdpProtocolPeerEvent Current remote frame %d new remote frame %d",current_remote_frame, new_remote_frame);
            }
            check(current_remote_frame == -1 || new_remote_frame == (current_remote_frame + 1));

            _sync.AddRemoteInput(queue, evt.u.input.input);
            // Notify the other endpoints which frame we received from a peer
			UE_LOG(GGPOLOG, Verbose, TEXT("Peer2PeerBackend::OnUdpProtocolPeerEvent setting remote connect status for queue %d to %d"), queue, evt.u.input.input.frame);

            _local_connect_status[queue].last_frame = evt.u.input.input.frame;
         }
         break;

   case UdpProtocol::Event::Disconnected:
      DisconnectPlayer(QueueToPlayerHandle(queue));
      break;
   }
}


void
Peer2PeerBackend::OnUdpProtocolSpectatorEvent(UdpProtocol::Event &evt, int queue)
{
   GGPOPlayerHandle handle = QueueToSpectatorHandle(queue);
   OnUdpProtocolEvent(evt, handle);

   GGPOEvent info;

   switch (evt.type) {
   case UdpProtocol::Event::Disconnected:
      _spectators[queue].Disconnect();

      info.code = GGPO_EVENTCODE_DISCONNECTED_FROM_PEER;
      info.u.disconnected.player = handle;
      _callbacks.on_event(&info);

      break;
   }
}

void
Peer2PeerBackend::OnUdpProtocolEvent(UdpProtocol::Event &evt, GGPOPlayerHandle handle)
{
   GGPOEvent info;

   switch (evt.type) {
   case UdpProtocol::Event::Connected:
      info.code = GGPO_EVENTCODE_CONNECTED_TO_PEER;
      info.u.connected.player = handle;
      _callbacks.on_event(&info);
      break;
   case UdpProtocol::Event::Synchronizing:
      info.code = GGPO_EVENTCODE_SYNCHRONIZING_WITH_PEER;
      info.u.synchronizing.player = handle;
      info.u.synchronizing.count = evt.u.synchronizing.count;
      info.u.synchronizing.total = evt.u.synchronizing.total;
      _callbacks.on_event(&info);
      break;
   case UdpProtocol::Event::Synchronzied:
      info.code = GGPO_EVENTCODE_SYNCHRONIZED_WITH_PEER;
      info.u.synchronized.player = handle;
      _callbacks.on_event(&info);

      CheckInitialSync();
      break;

   case UdpProtocol::Event::NetworkInterrupted:
      info.code = GGPO_EVENTCODE_CONNECTION_INTERRUPTED;
      info.u.connection_interrupted.player = handle;
      info.u.connection_interrupted.disconnect_timeout = evt.u.network_interrupted.disconnect_timeout;
      _callbacks.on_event(&info);
      break;

   case UdpProtocol::Event::NetworkResumed:
      info.code = GGPO_EVENTCODE_CONNECTION_RESUMED;
      info.u.connection_resumed.player = handle;
      _callbacks.on_event(&info);
      break;
   }
}

/*
 * Called only as the result of a local decision to disconnect.  The remote
 * decisions to disconnect are a result of us parsing the peer_connect_settings
 * blob in every endpoint periodically.
 */
GGPOErrorCode
Peer2PeerBackend::DisconnectPlayer(GGPOPlayerHandle player)
{
   int queue;
   GGPOErrorCode result;

   result = PlayerHandleToQueue(player, &queue);
   if (!GGPO_SUCCEEDED(result)) {
      return result;
   }
   
   if (_local_connect_status[queue].disconnected) {
      return GGPO_ERRORCODE_PLAYER_DISCONNECTED;
   }

   if (!_endpoints[queue].IsInitialized()) {
      int current_frame = _sync.GetFrameCount();
      // xxx: we should be tracking who the local player is, but for now assume
      // that if the endpoint is not initalized, this must be the local player.
	  UE_LOG(GGPOLOG, Verbose, TEXT("Peer2PeerBackend::DisconnectPlayer Disconnecting local player %d at frame %d by user request."), queue, _local_connect_status[queue].last_frame);
      for (int i = 0; i < _num_players; i++) {
         if (_endpoints[i].IsInitialized()) {
            DisconnectPlayerQueue(i, current_frame);
         }
      }
   } else {
	  UE_LOG(GGPOLOG, Verbose, TEXT("Peer2PeerBackend::DisconnectPlayer Disconnecting queue %d at frame %d by user request."), queue, _local_connect_status[queue].last_frame);
	  DisconnectPlayerQueue(queue, _local_connect_status[queue].last_frame);
   }
   return GGPO_OK;
}

void
Peer2PeerBackend::DisconnectPlayerQueue(int queue, int syncto)
{
   GGPOEvent info;
   int framecount = _sync.GetFrameCount();

   _endpoints[queue].Disconnect();

   UE_LOG(GGPOLOG, Verbose, TEXT("Peer2PeerBackend::DisconnectPlayerQueue Changing queue %d local connect status for last frame from %d to %d on disconnect request (current: %d)."),
	   queue, _local_connect_status[queue].last_frame, syncto, framecount);

   _local_connect_status[queue].disconnected = 1;
   _local_connect_status[queue].last_frame = syncto;

   if (syncto < framecount) {
	   UE_LOG(GGPOLOG, Verbose, TEXT("Peer2PeerBackend::DisconnectPlayerQueue adjusting simulation to account for the fact that %d disconnected @ %d."),
		   queue, syncto);
      _sync.AdjustSimulation(syncto);
	  UE_LOG(GGPOLOG, Verbose, TEXT("Peer2PeerBackend::DisconnectPlayerQueue finished adjusting simulation."));
   }

   info.code = GGPO_EVENTCODE_DISCONNECTED_FROM_PEER;
   info.u.disconnected.player = QueueToPlayerHandle(queue);
   _callbacks.on_event(&info);

   CheckInitialSync();
}


GGPOErrorCode
Peer2PeerBackend::GetNetworkStats(FGGPONetworkStats *stats, GGPOPlayerHandle player)
{
   int queue;
   GGPOErrorCode result;

   result = PlayerHandleToQueue(player, &queue);
   if (!GGPO_SUCCEEDED(result)) {
      return result;
   }

   memset(stats, 0, sizeof *stats);
   _endpoints[queue].GetNetworkStats(stats);

   return GGPO_OK;
}

GGPOErrorCode
Peer2PeerBackend::SetFrameDelay(GGPOPlayerHandle player, int delay) 
{ 
   int queue;
   GGPOErrorCode result;

   result = PlayerHandleToQueue(player, &queue);
   if (!GGPO_SUCCEEDED(result)) {
      return result;
   }
   _sync.SetFrameDelay(queue, delay);
   return GGPO_OK; 
}

GGPOErrorCode
Peer2PeerBackend::SetDisconnectTimeout(int timeout)
{
   _disconnect_timeout = timeout;
   for (int i = 0; i < _num_players; i++) {
      if (_endpoints[i].IsInitialized()) {
         _endpoints[i].SetDisconnectTimeout(_disconnect_timeout);
      }
   }
   return GGPO_OK;
}

GGPOErrorCode
Peer2PeerBackend::SetDisconnectNotifyStart(int timeout)
{
   _disconnect_notify_start = timeout;
   for (int i = 0; i < _num_players; i++) {
      if (_endpoints[i].IsInitialized()) {
         _endpoints[i].SetDisconnectNotifyStart(_disconnect_notify_start);
      }
   }
   return GGPO_OK;
}

GGPOErrorCode
Peer2PeerBackend::TrySynchronizeLocal()
{
    if (_num_players <= 1 && _num_spectators == 0) {
        // xxx: Same as below in CheckInitialSync(), IsInitialized() is used
        // to test "represents the local player"
        if (_num_players == 0 || !_endpoints[0].IsInitialized()) {
            CheckInitialSync();
        }
    }

    if (_synchronizing) {
        return GGPO_ERRORCODE_NOT_SYNCHRONIZED;
    }
	UE_LOG(GGPOLOG, Verbose, TEXT("Peer2PeerBackend::TrySynchronizeLocal Synchronized local-only simulation."));
    return GGPO_OK;
}

GGPOErrorCode
Peer2PeerBackend::PlayerHandleToQueue(GGPOPlayerHandle player, int *queue)
{
   int offset = ((int)player - 1);
   if (offset < 0 || offset >= _num_players) {
      return GGPO_ERRORCODE_INVALID_PLAYER_HANDLE;
   }
   *queue = offset;
   return GGPO_OK;
}

 
void
Peer2PeerBackend::OnMsg(int connection_id, UdpMsg *msg, int len)
{
   for (int i = 0; i < _num_players; i++) {
      if (_endpoints[i].HandlesMsg(connection_id, msg)) {
         _endpoints[i].OnMsg(msg, len);
         return;
      }
   }
   for (int i = 0; i < _num_spectators; i++) {
      if (_spectators[i].HandlesMsg(connection_id, msg)) {
         _spectators[i].OnMsg(msg, len);
         return;
      }
   }
}

void
Peer2PeerBackend::CheckInitialSync()
{
   int i;

   if (_synchronizing) {
      // Check to see if everyone is now synchronized.  If so,
      // go ahead and tell the client that we're ok to accept input.
      for (i = 0; i < _num_players; i++) {
         // xxx: IsInitialized() must go... we're actually using it as a proxy for "represents the local player"
         if (_endpoints[i].IsInitialized() && !_endpoints[i].IsSynchronized() && !_local_connect_status[i].disconnected) {
            return;
         }
      }
      for (i = 0; i < _num_spectators; i++) {
         if (_spectators[i].IsInitialized() && !_spectators[i].IsSynchronized()) {
            return;
         }
      }

      GGPOEvent info;
      info.code = GGPO_EVENTCODE_RUNNING;
      _callbacks.on_event(&info);
      _synchronizing = false;
   }
}