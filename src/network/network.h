/*
    Mosh: the mobile shell
    Copyright 2012 Keith Winstein

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give
    permission to link the code of portions of this program with the
    OpenSSL library under certain conditions as described in each
    individual source file, and distribute linked combinations including
    the two.

    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the
    file(s), but you are not obligated to do so. If you do not wish to do
    so, delete this exception statement from your version. If you delete
    this exception statement from all source files in the program, then
    also delete it here.
*/

#ifndef NETWORK_HPP
#define NETWORK_HPP

#include <stdint.h>
#include <deque>
#include <map>
#include <set>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string>
#include <math.h>
#include <vector>
#include <assert.h>

#include "crypto.h"
#include "addresses.h"

using namespace Crypto;

namespace Network {
  static const unsigned int MOSH_PROTOCOL_VERSION = 2; /* bumped for echo-ack */

  uint64_t timestamp( void );
  uint16_t timestamp16( void );
  uint16_t timestamp_diff( uint16_t tsnew, uint16_t tsold );

  class NetworkException {
  public:
    string function;
    int the_errno;
    NetworkException( string s_function, int s_errno ) : function( s_function ), the_errno( s_errno ) {}
    NetworkException() : function( "<none>" ), the_errno( 0 ) {}
  };

  enum Direction {
    TO_SERVER = 0,
    TO_CLIENT = 1
  };

  class Packet {
  public:
    uint64_t seq;
    Direction direction;
    uint16_t timestamp, timestamp_reply;
    uint16_t sock_id;
    uint16_t flags;
    string payload;

    Packet( uint64_t s_seq, Direction s_direction,
	    uint16_t s_timestamp, uint16_t s_timestamp_reply,
            uint16_t s_sock_id, uint16_t s_flags, string s_payload )
      : seq( s_seq ), direction( s_direction ),
	timestamp( s_timestamp ), timestamp_reply( s_timestamp_reply ),
        sock_id( s_sock_id ), flags( s_flags ), payload( s_payload )
    {}

    Packet( string coded_packet, Session *session );

    bool is_probe( void );
    bool is_addr_msg( void );
    string tostring( Session *session );
  };

  class Connection {
  private:
    static const int DEFAULT_SEND_MTU = 1300;
    static const uint64_t MIN_RTO = 50; /* ms */
    static const uint64_t MAX_RTO = 1000; /* ms */
    static const uint64_t MIN_PROBE_INTERVAL = 1000;

    static const int PORT_RANGE_LOW  = 60001;
    static const int PORT_RANGE_HIGH = 60999;

    static const unsigned int SERVER_ASSOCIATION_TIMEOUT = 40000;
    static const unsigned int PORT_HOP_INTERVAL          = 10000;

    static const unsigned int MAX_PORTS_OPEN             = 10; /* by active sockets (see prune_sockets()) */
    static const unsigned int MAX_OLD_SOCKET_AGE         = 60000;

    static const int CONGESTION_TIMESTAMP_PENALTY = 500; /* ms */

    bool try_bind( const char *addr, int port_low, int port_high );
    bool try_bind( const Addr &local_addr );

    class Socket
    {
    private:
      int _fd;
      void socket_init( int lower_port, int higher_port );

    public:
      int MTU;
      uint16_t saved_timestamp;
      uint64_t saved_timestamp_received_at;
      bool RTT_hit;
      double SRTT;
      double RTTVAR;
      /* The message nonce should also have a Socket ID.  Otherwise, if a data
         packet A is sent, and then a probe B, A will be discarded if B arrives
         first.  Same for sending B then A: B is discarded. */
      uint64_t next_seq;
      uint16_t sock_id;
      Addr local_addr;
      Addr remote_addr;

      int fd( void ) const { return _fd; }
      static bool srtt_order( Socket *s1, Socket *s2 ) { return s1->SRTT < s2->SRTT; }
      static bool addr_order( Socket *s1, Socket *s2 ) { return s1->local_addr < s2->local_addr; }
      Socket( Addr addr_to_bind, int lower_port, int higher_port, Addr remote_addr, uint16_t id );
      Socket( Socket *old );
      ~Socket();

      Socket( const Socket & other );
      Socket & operator=( const Socket & other );
    };

    std::deque< Socket* > socks;
    std::deque< Socket* > old_socks; /* Read-only (post hop-port) */
    uint16_t next_sock_id;
    Socket *send_socket;
    bool has_remote_addr( void ) const { return send_socket != NULL; };
    std::vector< Addr > remote_addr;
    std::vector< Addr > received_remote_addr;
    Addresses host_addresses;
    bool rebind;

    bool server;

    Base64Key key;
    Session session;

    void setup( void );

    Direction direction;
    /* Associate a remote sock ID to its receiver seq. */
    std::map< uint16_t, uint64_t > expected_receiver_seq;

    uint64_t last_heard;
    uint64_t last_port_choice;
    uint64_t last_roundtrip_success; /* transport layer needs to tell us this */

    /* Exception from send(), to be delivered if the frontend asks for it,
       without altering control flow. */
    bool have_send_exception;
    NetworkException send_exception;

    Packet new_packet( Socket *sock, uint16_t probe, string &s_payload );

    void hop_port( void );

    Socket *sock( void ) const { assert( !socks.empty() ); return send_socket ? send_socket : socks.back(); }
    void refill_socks( std::set< Addr > &addresses );
    void prune_sockets( void );

    void send_addresses( void );
    void parse_received_addresses( string payload );
    bool send_probe( Socket *sock, Addr &addr );
    string recv_one( Socket *sock_to_recv, bool nonblocking );

  public:
    Connection( const char *desired_ip, const char *desired_port ); /* server */
    Connection( const char *key_str, const char *ip, const char *port ); /* client */
    ~Connection();

    void send( uint16_t flags, string s );
    void send_probes( void );
    string recv( void );
    const std::vector< int > fds( void ) const;
    int get_MTU( void ) const { return send_socket->MTU; }

    std::string port( void ) const;
    string get_key( void ) const { return key.printable_key(); }
    bool get_has_remote_addr( void ) const { return has_remote_addr(); }

    uint64_t timeout( void ) const;
    double get_SRTT( void ) const { return send_socket ? send_socket->SRTT : socks.back()->SRTT; }

    const Addr &get_remote_addr( void ) const { return ( send_socket ? send_socket : socks.back() )->remote_addr; }
    socklen_t get_remote_addr_len( void ) const { return send_socket ? send_socket->remote_addr.addrlen : 0; }

    const NetworkException *get_send_exception( void ) const
    {
      return have_send_exception ? &send_exception : NULL;
    }

    void set_last_roundtrip_success( uint64_t s_success ) { last_roundtrip_success = s_success; }

    static bool parse_portrange( const char * desired_port_range, int & desired_port_low, int & desired_port_high );
  };
}

#endif
