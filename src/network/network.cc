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

#include "config.h"
#include "debug.h"

#include <sys/types.h>
#include <sys/socket.h>
#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif
#include <netdb.h>
#include <netinet/in.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/ioctl.h>


#include "dos_assert.h"
#include "fatal_assert.h"
#include "byteorder.h"
#include "network.h"
#include "crypto.h"

#include "timestamp.h"

#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT MSG_NONBLOCK
#endif

using namespace std;
using namespace Network;
using namespace Crypto;

const uint64_t DIRECTION_MASK = uint64_t(1) << 63;
const uint64_t SEQUENCE_MASK = uint64_t(-1) ^ DIRECTION_MASK;
const uint16_t PROBE_FLAG = 1;

/* Read in packet from coded string */
Packet::Packet( string coded_packet, Session *session )
  : seq( -1 ),
    direction( TO_SERVER ),
    timestamp( -1 ),
    timestamp_reply( -1 ),
    payload()
{
  Message message = session->decrypt( coded_packet );

  direction = (message.nonce.val() & DIRECTION_MASK) ? TO_CLIENT : TO_SERVER;
  seq = message.nonce.val() & SEQUENCE_MASK;

  dos_assert( message.text.size() >= 4 * sizeof( uint16_t ) );

  uint16_t *data = (uint16_t *)message.text.data();
  timestamp = be16toh( data[ 0 ] );
  timestamp_reply = be16toh( data[ 1 ] );
  sock_id = be16toh( data[2] );
  flags = be16toh( data[3] );

  payload = string( message.text.begin() + 4 * sizeof( uint16_t ), message.text.end() );
}

bool Packet::is_probe( void )
{
  return flags & PROBE_FLAG;
}

/* Output coded string from packet */
string Packet::tostring( Session *session )
{
  uint64_t direction_seq = (uint64_t( direction == TO_CLIENT ) << 63) | (seq & SEQUENCE_MASK);

  uint16_t ts_net[ 2 ] = { static_cast<uint16_t>( htobe16( timestamp ) ),
                           static_cast<uint16_t>( htobe16( timestamp_reply ) ) };
  uint16_t id_net = static_cast<uint16_t>( htobe16( sock_id ) );
  uint16_t flags_net = static_cast<uint16_t>( htobe16( flags ) );

  string timestamps = string( (char *)ts_net, 2 * sizeof( uint16_t ) );
  string id_string = string( (char *)&id_net, sizeof( uint16_t ) );
  string flags_string = string( (char *)&flags_net, sizeof( uint16_t ) );

  return session->encrypt( Message( Nonce( direction_seq ), timestamps + id_string + flags_string + payload ) );
}

Packet Connection::new_packet( Socket *sock, uint16_t flags, string &s_payload )
{
  uint16_t outgoing_timestamp_reply = -1;

  uint64_t now = timestamp();

  if ( now - sock->saved_timestamp_received_at < 1000 ) { /* we have a recent received timestamp */
    /* send "corrected" timestamp advanced by how long we held it */
    outgoing_timestamp_reply = sock->saved_timestamp + (now - sock->saved_timestamp_received_at);
    sock->saved_timestamp = -1;
    sock->saved_timestamp_received_at = 0;
  }

  Packet p( sock->next_seq++, direction, timestamp16(), outgoing_timestamp_reply,
	    sock->sock_id, flags, s_payload );

  return p;
}
#include <arpa/inet.h>

std::set< Addr > Connection::get_host_addresses( void )
{
  int ioctl_sock, rc;
  struct ifconf ifconf;
  char buffer[2048];
  char *req_off, *end_off;
  struct ifreq *req;
  std::set< Addr >addresses;

  ioctl_sock = socket( AF_INET, SOCK_DGRAM, 0 );
  if ( ioctl_sock < 0 ) {
    perror("Cannot get host addresses (socket), will only try with a default one");
    return addresses;
  }

  memset( &ifconf, 0, sizeof( ifconf ) );
  ifconf.ifc_buf = buffer;
  /* we could know the needed buffer length with ifconf.ifc_buf = NULL */
  ifconf.ifc_len = sizeof( buffer );
  memset( buffer, 0, sizeof( buffer ) );

  rc = ioctl( ioctl_sock, SIOCGIFCONF, &ifconf );
  if ( rc < 0 ) {
    perror("Cannot get host addresses (ioctl), will only try with a default one");
    goto end;
  }

  end_off = buffer + ifconf.ifc_len;
  if ( buffer + 2048 <= end_off - (IFNAMSIZ + sizeof( struct ifreq )) ) {
    fprintf(stderr, "Warning, some addresses may be missing (buffer size, please report).\n");
  }

  req = ifconf.ifc_ifcu.ifcu_req;
  req_off = (char *) req;
  while ( req_off + IFNAMSIZ + sizeof( req->ifr_addr.sa_len ) < end_off &&
	  req_off + IFNAMSIZ + req->ifr_addr.sa_len < end_off) {
    /* Should filter loopback addresses, link-local addresses, and so on. */
    if ( req->ifr_addr.sa_family == AF_INET || req->ifr_addr.sa_family == AF_INET6 ) {
      addresses.insert( Addr( req->ifr_addr ) );
    }
    req_off += IFNAMSIZ + req->ifr_addr.sa_len;
    req = (struct ifreq *) req_off;
  }

  /* debug (to be deleted) */
  fprintf( pok, "%d addresses found :", (int) addresses.size() );
  for ( std::set< Addr >::const_iterator it = addresses.begin();
	it != addresses.end();
	it++ ) {
    const struct sockaddr *saddr = &it->sa;
    char addrname[INET6_ADDRSTRLEN];
    int family = saddr->sa_family;
    const char *tmp;
    void *addr;
    if (family == AF_INET) {
        addr = &((struct sockaddr_in*)saddr)->sin_addr;
    } else if (family == AF_INET6) {
        addr = &((struct sockaddr_in6*)saddr)->sin6_addr;
    } else {
        fprintf(pok, "unknown address family: %d\n", family);
        continue;
    }
    tmp = inet_ntop(family, addr, addrname, INET6_ADDRSTRLEN);
    if (!tmp)
        fprintf( pok, "can't print addr");
    else
        fprintf( pok, "%s\n", tmp);
  } /* end: to be deleted */

 end:
  close( ioctl_sock );
  return addresses;
}

void Connection::hop_port( void )
{
  // std::set< Addr > addresses = get_host_addresses();
  assert( !server );

  setup();
  assert( remote_addr_len != 0 );
  Socket *sock = new Socket( remote_addr.sa.sa_family, next_sock_id++ );
  socks.push_back( sock );

  prune_sockets();
}

void Connection::prune_sockets( void )
{
  /* don't keep old sockets if the new socket has been working for long enough */
  if ( socks.size() > 1 ) {
    if ( timestamp() - last_port_choice > MAX_OLD_SOCKET_AGE ) {
      int num_to_kill = socks.size() - 1;
      for ( int i = 0; i < num_to_kill; i++ ) {
	socks.pop_front();
      }
    }
  } else {
    return;
  }

  /* make sure we don't have too many receive sockets open */
  if ( socks.size() > MAX_PORTS_OPEN ) {
    int num_to_kill = socks.size() - MAX_PORTS_OPEN;
    for ( int i = 0; i < num_to_kill; i++ ) {
      socks.pop_front();
    }
  }
}

Connection::Socket::Socket( int family, uint16_t id )
    : _fd( socket( family, SOCK_DGRAM, 0 ) ),
    MTU( DEFAULT_SEND_MTU ),
    saved_timestamp( -1 ),
    saved_timestamp_received_at( 0 ),
    RTT_hit( false ),
    SRTT( 1000 ),
    RTTVAR( 500 ),
    next_seq( 0 ),
    sock_id( id )
{
  if ( _fd < 0 ) {
    throw NetworkException( "socket", errno );
  }

  /* Disable path MTU discovery */
#ifdef HAVE_IP_MTU_DISCOVER
  char flag = IP_PMTUDISC_DONT;
  socklen_t optlen = sizeof( flag );
  if ( setsockopt( _fd, IPPROTO_IP, IP_MTU_DISCOVER, &flag, optlen ) < 0 ) {
    throw NetworkException( "setsockopt", errno );
  }
#endif

  //  int dscp = 0x92; /* OS X does not have IPTOS_DSCP_AF42 constant */
  int dscp = 0x02; /* ECN-capable transport only */
  if ( setsockopt( _fd, IPPROTO_IP, IP_TOS, &dscp, sizeof (dscp)) < 0 ) {
    //    perror( "setsockopt( IP_TOS )" );
  }

  /* request explicit congestion notification on received datagrams */
#ifdef HAVE_IP_RECVTOS
  int tosflag = true;
  socklen_t tosoptlen = sizeof( tosflag );
  if ( setsockopt( _fd, IPPROTO_IP, IP_RECVTOS, &tosflag, tosoptlen ) < 0 ) {
    perror( "setsockopt( IP_RECVTOS )" );
  }
#endif
}

void Connection::setup( void )
{
  last_port_choice = timestamp();
}

const std::vector< int > Connection::fds( void ) const
{
  std::vector< int > ret;

  for ( std::deque< Socket* >::const_iterator it = socks.begin();
	it != socks.end();
	it++ ) {
    ret.push_back( (*it)->fd() );
  }

  return ret;
}

class AddrInfo {
public:
  struct addrinfo *res;
  AddrInfo( const char *node, const char *service,
	    const struct addrinfo *hints ) :
    res( NULL ) {
    int errcode = getaddrinfo( node, service, hints, &res );
    if ( errcode != 0 ) {
      throw NetworkException( std::string( "Bad IP address (" ) + (node != NULL ? node : "(null)") + "): " + gai_strerror( errcode ), 0 );
    }
  }
  ~AddrInfo() { freeaddrinfo(res); }
private:
  AddrInfo(const AddrInfo &);
  AddrInfo &operator=(const AddrInfo &);
};

Connection::Connection( const char *desired_ip, const char *desired_port ) /* server */
  : socks(),
    next_sock_id( 0 ),
    send_socket( NULL ),
    remote_addr(),
    remote_addr_len( 0 ),
    server( true ),
    key(),
    session( key ),
    direction( TO_CLIENT ),
    expected_receiver_seq(),
    last_heard( -1 ),
    last_port_choice( -1 ),
    last_roundtrip_success( -1 ),
    have_send_exception( false ),
    send_exception()
{
  setup();

  /* The mosh wrapper always gives an IP request, in order
     to deal with multihomed servers. The port is optional. */

  /* If an IP request is given, we try to bind to that IP, but we also
     try INADDR_ANY. If a port request is given, we bind only to that port. */

  /* convert port numbers */
  int desired_port_low = 0;
  int desired_port_high = 0;

  if ( desired_port && !parse_portrange( desired_port, desired_port_low, desired_port_high ) ) {
    throw NetworkException("Invalid port range", 0);
  }

  /* try to bind to desired IP first */
  if ( desired_ip ) {
    try {
      if ( try_bind( desired_ip, desired_port_low, desired_port_high ) ) { return; }
    } catch ( const NetworkException& e ) {
      fprintf( stderr, "Error binding to IP %s: %s: %s\n",
	       desired_ip,
	       e.function.c_str(), strerror( e.the_errno ) );
    }
  }

  /* now try any local interface */
  try {
    if ( try_bind( NULL, desired_port_low, desired_port_high ) ) { return; }
  } catch ( const NetworkException& e ) {
    fprintf( stderr, "Error binding to any interface: %s: %s\n",
	     e.function.c_str(), strerror( e.the_errno ) );
    throw; /* this time it's fatal */
  }

  assert( false );
  throw NetworkException( "Could not bind", errno );
}

bool Connection::try_bind( const char *addr, int port_low, int port_high )
{
  struct addrinfo hints;
  memset( &hints, 0, sizeof( hints ) );
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST | AI_NUMERICSERV;
  AddrInfo ai( addr, "0", &hints );

  Addr local_addr;
  socklen_t local_addr_len = ai.res->ai_addrlen;
  memcpy( &local_addr.sa, ai.res->ai_addr, local_addr_len );

  int search_low = PORT_RANGE_LOW, search_high = PORT_RANGE_HIGH;

  if ( port_low != 0 ) { /* low port preference */
    search_low = port_low;
  }
  if ( port_high != 0 ) { /* high port preference */
    search_high = port_high;
  }

  Socket *sock_tmp = new Socket( local_addr.sa.sa_family, next_sock_id++ );
  socks.push_back( sock_tmp );
  for ( int i = search_low; i <= search_high; i++ ) {
    switch (local_addr.sa.sa_family) {
    case AF_INET:
      local_addr.sin.sin_port = htons( i );
      break;
    case AF_INET6:
      local_addr.sin6.sin6_port = htons( i );
      break;
    default:
      throw NetworkException( "Unknown address family", 0 );
    }

    if ( bind( sock_tmp->fd(), &local_addr.sa, local_addr_len ) == 0 ) {
      return true;
    } else if ( i == search_high ) { /* last port to search */
      int saved_errno = errno;
      socks.pop_back();
      delete sock_tmp;
      char host[ NI_MAXHOST ], serv[ NI_MAXSERV ];
      int errcode = getnameinfo( &local_addr.sa, local_addr_len,
				 host, sizeof( host ), serv, sizeof( serv ),
				 NI_DGRAM | NI_NUMERICHOST | NI_NUMERICSERV );
      if ( errcode != 0 ) {
	throw NetworkException( std::string( "bind: getnameinfo: " ) + gai_strerror( errcode ), 0 );
      }
      fprintf( stderr, "Failed binding to %s:%s\n",
	       host, serv );
      throw NetworkException( "bind", saved_errno );
    }
  }

  assert( false );
  return false;
}

Connection::Connection( const char *key_str, const char *ip, const char *port ) /* client */
  : socks(),
    next_sock_id( 0 ),
    send_socket( NULL ),
    remote_addr(),
    remote_addr_len( 0 ),
    server( false ),
    key( key_str ),
    session( key ),
    direction( TO_SERVER ),
    expected_receiver_seq(),
    last_heard( -1 ),
    last_port_choice( -1 ),
    last_roundtrip_success( -1 ),
    have_send_exception( false ),
    send_exception()
{
  setup();

  /* associate socket with remote host and port */
  struct addrinfo hints;
  memset( &hints, 0, sizeof( hints ) );
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
  AddrInfo ai( ip, port, &hints );
  fatal_assert( ai.res->ai_addrlen <= sizeof( remote_addr ) );
  remote_addr_len = ai.res->ai_addrlen;
  memcpy( &remote_addr.sa, ai.res->ai_addr, remote_addr_len );

  send_socket = new Socket( remote_addr.sa.sa_family, next_sock_id++ );

  socks.push_back( send_socket );
}

void Connection::send_probes( void )
{
  bool has_fail;
  for ( std::deque< Socket* >::iterator it = socks.begin();
	it != socks.end();
	it++ ) {
    if ( *it != send_socket ) {
      bool rc = send_probe( *it, &remote_addr, remote_addr_len );
      has_fail = has_fail || rc;
    }
  }

  if ( has_fail ) {
    /* Mt: recheck interfaces. */
  }
}

bool Connection::send_probe( Socket *sock, Addr *addr, socklen_t addr_len )
{
  string empty("");
  Packet px = new_packet( sock, PROBE_FLAG, empty );
  // sock->last_probe = ( sock->last_probe + 1 ) & PROBE_NUM;

  string p = px.tostring( &session );

  fprintf(pok, "Probe sent.\n");
  fflush(pok);
  ssize_t bytes_sent = sendto( sock->fd(), p.data(), p.size(), MSG_DONTWAIT,
			       &addr->sa, addr_len );

  return ( bytes_sent != static_cast<ssize_t>( p.size() ) );
}

void Connection::send( string s )
{
  if ( !has_remote_addr() ) {
    return;
  }

  Packet px = new_packet( send_socket, 0, s );

  string p = px.tostring( &session );

  fprintf(pok, "send data\n");
  fflush(pok);
  ssize_t bytes_sent = sendto( sock()->fd(), p.data(), p.size(), MSG_DONTWAIT,
			       &remote_addr.sa, remote_addr_len );

  if ( bytes_sent == static_cast<ssize_t>( p.size() ) ) {
    have_send_exception = false;
  } else {
    /* Notify the frontend on sendto() failure, but don't alter control flow.
       sendto() success is not very meaningful because packets can be lost in
       flight anyway. */
    have_send_exception = true;
    send_exception = NetworkException( "sendto", errno );

    if ( errno == EMSGSIZE ) {
      send_socket->MTU = 500; /* payload MTU of last resort */
    }
  }

  uint64_t now = timestamp();
  if ( server ) {
    if ( now - last_heard > SERVER_ASSOCIATION_TIMEOUT ) {
      send_socket = NULL;
      fprintf( stderr, "Server now detached from client.\n" );
    }
  } else { /* client */
    if ( ( now - last_port_choice > PORT_HOP_INTERVAL )
	 && ( now - last_roundtrip_success > PORT_HOP_INTERVAL ) ) {
      hop_port();
    }
  }
}

string Connection::recv( void )
{
  assert( !socks.empty() );
  for ( std::deque< Socket* >::iterator it = socks.begin();
	it != socks.end();
	it++ ) {
    bool islast = (it + 1) == socks.end();
    string payload;
    try {
      payload = recv_one( *it, !islast );
    } catch ( NetworkException & e ) {
      if ( (e.the_errno == EAGAIN)
	   || (e.the_errno == EWOULDBLOCK) ) {
	assert( !islast );
	continue;
      } else {
	throw;
      }
    }

    /* succeeded */
    prune_sockets();
    return payload;
  }
  assert( false );
  return "";
}

string Connection::recv_one( Socket *sock, bool nonblocking )
{
  int sock_to_recv = sock->fd();
  /* receive source address, ECN, and payload in msghdr structure */
  Addr packet_remote_addr;
  struct msghdr header;
  struct iovec msg_iovec;

  char msg_payload[ Session::RECEIVE_MTU ];
  char msg_control[ Session::RECEIVE_MTU ];

  /* receive source address */
  header.msg_name = &packet_remote_addr.sa;
  header.msg_namelen = sizeof( packet_remote_addr );

  /* receive payload */
  msg_iovec.iov_base = msg_payload;
  msg_iovec.iov_len = Session::RECEIVE_MTU;
  header.msg_iov = &msg_iovec;
  header.msg_iovlen = 1;

  /* receive explicit congestion notification */
  header.msg_control = msg_control;
  header.msg_controllen = Session::RECEIVE_MTU;

  /* receive flags */
  header.msg_flags = 0;

  ssize_t received_len = recvmsg( sock_to_recv, &header, nonblocking ? MSG_DONTWAIT : 0 );

  if ( received_len < 0 ) {
    throw NetworkException( "recvmsg", errno );
  }

  if ( header.msg_flags & MSG_TRUNC ) {
    throw NetworkException( "Received oversize datagram", errno );
  }

  /* receive ECN */
  bool congestion_experienced = false;

  struct cmsghdr *ecn_hdr = CMSG_FIRSTHDR( &header );
  if ( ecn_hdr
       && (ecn_hdr->cmsg_level == IPPROTO_IP)
       && (ecn_hdr->cmsg_type == IP_TOS) ) {
    /* got one */
    uint8_t *ecn_octet_p = (uint8_t *)CMSG_DATA( ecn_hdr );
    assert( ecn_octet_p );

    if ( (*ecn_octet_p & 0x03) == 0x03 ) {
      congestion_experienced = true;
    }
  }

  Packet p( string( msg_payload, received_len ), &session );

  dos_assert( p.direction == (server ? TO_SERVER : TO_CLIENT) ); /* prevent malicious playback to sender */

  if ( p.seq >= expected_receiver_seq[p.sock_id] ) { /* don't use out-of-order packets for timestamp or targeting */
    expected_receiver_seq[p.sock_id] = p.seq + 1; /* this is security-sensitive because a replay attack could otherwise
						     screw up the timestamp and targeting */

    if ( p.timestamp != uint16_t(-1) ) {
      sock->saved_timestamp = p.timestamp;
      sock->saved_timestamp_received_at = timestamp();

      if ( congestion_experienced ) {
	/* signal counterparty to slow down */
	/* this will gradually slow the counterparty down to the minimum frame rate */
	sock->saved_timestamp -= CONGESTION_TIMESTAMP_PENALTY;
	if ( server ) {
	  fprintf( stderr, "Received explicit congestion notification.\n" );
	}
      }
    }

    if ( p.timestamp_reply != uint16_t(-1) ) {
      uint16_t now = timestamp16();
      double R = timestamp_diff( now, p.timestamp_reply );

      if ( R < 5000 ) { /* ignore large values, e.g. server was Ctrl-Zed */
	if ( !sock->RTT_hit ) { /* first measurement */
	  sock->SRTT = R;
	  sock->RTTVAR = R / 2;
	  sock->RTT_hit = true;
	} else {
	  const double alpha = 1.0 / 8.0;
	  const double beta = 1.0 / 4.0;
	  
	  sock->RTTVAR = (1 - beta) * sock->RTTVAR + ( beta * fabs( sock->SRTT - R ) );
	  sock->SRTT = (1 - alpha) * sock->SRTT + ( alpha * R );
	}
      }
      if ( p.is_probe() ) {
	fprintf( pok, "Probe received, RTT=%u, SRTT=%u\n", (unsigned int)R, (unsigned int)sock->SRTT );
      }
    }

    /* auto-adjust to remote host */
    last_heard = timestamp();
    if ( p.is_probe() ) {
      if ( server ) {
	send_probe( sock, &packet_remote_addr, remote_addr_len );
      }
      if ( ! p.payload.empty() ) {
	fprintf(stderr, "Strange: probe with payload received.\n");
      }
      return p.payload;
    }

    if ( server ) { /* only client can roam */
      send_socket = sock;
      if ( remote_addr_len != header.msg_namelen ||
	   memcmp( &remote_addr, &packet_remote_addr, remote_addr_len ) != 0 ) {
	remote_addr = packet_remote_addr;
	remote_addr_len = header.msg_namelen;
	char host[ NI_MAXHOST ], serv[ NI_MAXSERV ];
	int errcode = getnameinfo( &remote_addr.sa, remote_addr_len,
				   host, sizeof( host ), serv, sizeof( serv ),
				   NI_DGRAM | NI_NUMERICHOST | NI_NUMERICSERV );
	if ( errcode != 0 ) {
	  throw NetworkException( std::string( "recv_one: getnameinfo: " ) + gai_strerror( errcode ), 0 );
	}
	fprintf( stderr, "Server now attached to client at %s:%s\n",
		 host, serv );
      }
    }
  }

  return p.payload; /* we do return out-of-order or duplicated packets to caller */
}

std::string Connection::port( void ) const
{
  Addr local_addr;
  socklen_t addrlen = sizeof( local_addr );

  if ( getsockname( sock()->fd(), &local_addr.sa, &addrlen ) < 0 ) {
    throw NetworkException( "getsockname", errno );
  }

  char serv[ NI_MAXSERV ];
  int errcode = getnameinfo( &local_addr.sa, addrlen,
			     NULL, 0, serv, sizeof( serv ),
			     NI_DGRAM | NI_NUMERICSERV );
  if ( errcode != 0 ) {
    throw NetworkException( std::string( "port: getnameinfo: " ) + gai_strerror( errcode ), 0 );
  }

  return std::string( serv );
}

uint64_t Network::timestamp( void )
{
  return frozen_timestamp();
}

uint16_t Network::timestamp16( void )
{
  uint16_t ts = timestamp() % 65536;
  if ( ts == uint16_t(-1) ) {
    ts++;
  }
  return ts;
}

uint16_t Network::timestamp_diff( uint16_t tsnew, uint16_t tsold )
{
  int diff = tsnew - tsold;
  if ( diff < 0 ) {
    diff += 65536;
  }
  
  assert( diff >= 0 );
  assert( diff <= 65535 );

  return diff;
}

uint64_t Connection::timeout( void ) const
{
  uint64_t RTO = lrint( ceil( sock()->SRTT + 4 * sock()->RTTVAR ) );
  if ( RTO < MIN_RTO ) {
    RTO = MIN_RTO;
  } else if ( RTO > MAX_RTO ) {
    RTO = MAX_RTO;
  }
  return RTO;
}

Connection::~Connection()
{
    for ( std::deque< Socket* >::iterator it = socks.begin();
	  it != socks.end();
	  it ++ ) {
      delete *it;
    }
}

Connection::Socket::~Socket()
{
  if ( close( _fd ) < 0 ) {
    throw NetworkException( "close", errno );
  }
}

Connection::Socket::Socket( const Socket & other )
  : _fd( dup( other._fd ) )
{
  if ( _fd < 0 ) {
    throw NetworkException( "socket", errno );
  }
}

Connection::Socket & Connection::Socket::operator=( const Socket & other )
{
  if ( dup2( other._fd, _fd ) < 0 ) {
    throw NetworkException( "socket", errno );
  }

  return *this;
}

bool Connection::parse_portrange( const char * desired_port, int & desired_port_low, int & desired_port_high )
{
  /* parse "port" or "portlow:porthigh" */
  desired_port_low = desired_port_high = 0;
  char *end;
  long value;

  /* parse first (only?) port */
  errno = 0;
  value = strtol( desired_port, &end, 10 );
  if ( (errno != 0) || (*end != '\0' && *end != ':') ) {
    fprintf( stderr, "Invalid (low) port number (%s)\n", desired_port );
    return false;
  }
  if ( (value < 0) || (value > 65535) ) {
    fprintf( stderr, "(Low) port number %ld outside valid range [0..65535]\n", value );
    return false;
  }

  desired_port_low = (int)value;
  if (*end == '\0') { /* not a port range */
    desired_port_high = desired_port_low;
    return true;
  }

  /* port range; parse high port */
  const char * cp = end + 1;
  errno = 0;
  value = strtol( cp, &end, 10 );
  if ( (errno != 0) || (*end != '\0') ) {
    fprintf( stderr, "Invalid high port number (%s)\n", cp );
    return false;
  }
  if ( (value < 0) || (value > 65535) ) {
    fprintf( stderr, "High port number %ld outside valid range [0..65535]\n", value );
    return false;
  }

  desired_port_high = (int)value;
  if ( desired_port_low > desired_port_high ) {
    fprintf( stderr, "Low port %d greater than high port %d\n", desired_port_low, desired_port_high );
    return false;
  }

  return true;
}
