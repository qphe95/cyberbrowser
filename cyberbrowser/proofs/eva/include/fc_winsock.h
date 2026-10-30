/* fc_winsock.h - Winsock 2, for Eva.  Part of windows.h's model: TRUSTED,
 * see there.  Included by it only (winsock2.h and ws2tcpip.h are windows.h).
 * Written out rather than taken from Frama-C's POSIX headers: a SOCKET is
 * an unsigned pointer-sized handle here, not an int. */
#ifndef FC_WINSOCK_H
#define FC_WINSOCK_H

typedef UINT_PTR SOCKET;
typedef HANDLE WSAEVENT;
#define INVALID_SOCKET ((SOCKET)(~0))
#define SOCKET_ERROR (-1)
#define WSA_INVALID_EVENT ((WSAEVENT)NULL)
#define WSAEWOULDBLOCK 10035L

typedef unsigned short ADDRESS_FAMILY, u_short;
typedef unsigned long u_long;
#define AF_UNSPEC 0
#define AF_INET 2
#define AF_INET6 23
#define SOCK_STREAM 1
#define IPPROTO_TCP 6
#define TCP_NODELAY 0x0001
#define SOL_SOCKET 0xffff
#define INADDR_LOOPBACK 0x7f000001

struct in_addr { union { struct { u_long s_addr; } S_un_l; u_long S_addr; } S_un; };
#define s_addr S_un.S_addr
struct in6_addr { union { unsigned char Byte[16]; unsigned short Word[8]; } u; };
#define s6_addr u.Byte
struct sockaddr { ADDRESS_FAMILY sa_family; char sa_data[14]; };
struct sockaddr_in {
    ADDRESS_FAMILY sin_family;
    unsigned short sin_port;
    struct in_addr sin_addr;
    char sin_zero[8];
};
struct sockaddr_in6 {
    ADDRESS_FAMILY sin6_family;
    unsigned short sin6_port;
    unsigned long sin6_flowinfo;
    struct in6_addr sin6_addr;
    unsigned long sin6_scope_id;
};
struct sockaddr_storage {
    ADDRESS_FAMILY ss_family;
    char __ss_pad1[6];
    long long __ss_align;
    char __ss_pad2[112];
};
typedef int socklen_t;
extern const struct in6_addr in6addr_loopback;

/* Byte order: what the functions compute, exactly (little-endian x64). */
static inline unsigned short htons(unsigned short v) { return (unsigned short)((v << 8) | (v >> 8)); }
static inline unsigned short ntohs(unsigned short v) { return (unsigned short)((v << 8) | (v >> 8)); }
static inline u_long htonl(u_long v) {
    return ((v & 0xffu) << 24) | ((v & 0xff00u) << 8) | ((v >> 8) & 0xff00u) | (v >> 24);
}
static inline u_long ntohl(u_long v) { return htonl(v); }

typedef struct WSAData {
    WORD wVersion, wHighVersion;
    unsigned short iMaxSockets, iMaxUdpDg;
    char *lpVendorInfo;
    char szDescription[257];
    char szSystemStatus[129];
} WSADATA, *LPWSADATA;

#define FD_READ_BIT 0
#define FD_READ (1 << FD_READ_BIT)
#define FD_WRITE_BIT 1
#define FD_WRITE (1 << FD_WRITE_BIT)
#define FD_CONNECT_BIT 4
#define FD_CONNECT (1 << FD_CONNECT_BIT)
#define FD_CLOSE_BIT 5
#define FD_CLOSE (1 << FD_CLOSE_BIT)
#define FD_MAX_EVENTS 10
typedef struct _WSANETWORKEVENTS {
    long lNetworkEvents;
    int iErrorCode[FD_MAX_EVENTS];
} WSANETWORKEVENTS, *LPWSANETWORKEVENTS;

/*@ requires out: \valid(lpWSAData);
    assigns *lpWSAData \from __fc_win_state, wVersionRequested;
    assigns __fc_win_state \from __fc_win_state;
    assigns \result \from __fc_win_state; */
int WSAStartup(WORD wVersionRequested, LPWSADATA lpWSAData);

/*@ assigns \result \from __fc_win_state; */
int WSAGetLastError(void);

/*@ assigns __fc_win_state \from __fc_win_state;
    assigns \result \from __fc_win_state; */
WSAEVENT WSACreateEvent(void);

/*@ assigns __fc_win_state \from __fc_win_state, hEvent;
    assigns \result \from __fc_win_state; */
BOOL WSACloseEvent(WSAEVENT hEvent);

/*@ assigns __fc_win_state \from __fc_win_state, s, hEventObject, lNetworkEvents;
    assigns \result \from __fc_win_state; */
int WSAEventSelect(SOCKET s, WSAEVENT hEventObject, long lNetworkEvents);

/*@ requires out: \valid(lpNetworkEvents);
    assigns *lpNetworkEvents \from __fc_win_state, s, hEventObject;
    assigns __fc_win_state \from __fc_win_state, s, hEventObject;
    assigns \result \from __fc_win_state; */
int WSAEnumNetworkEvents(SOCKET s, WSAEVENT hEventObject, LPWSANETWORKEVENTS lpNetworkEvents);

/*@ assigns __fc_win_state \from __fc_win_state, af, type, protocol;
    assigns \result \from __fc_win_state; */
SOCKET socket(int af, int type, int protocol);

/*@ assigns __fc_win_state \from __fc_win_state, s;
    assigns \result \from __fc_win_state; */
int closesocket(SOCKET s);

/*@ requires val: \valid_read(optval + (0 .. optlen - 1)) && \initialized(optval + (0 .. optlen - 1));
    assigns __fc_win_state \from __fc_win_state, s, level, optname, optval[0 .. optlen - 1];
    assigns \result \from __fc_win_state; */
int setsockopt(SOCKET s, int level, int optname, const char *optval, int optlen);

/*@ requires addr: namelen > 0 && \valid_read((char *)name + (0 .. namelen - 1));
    requires addr_set: \initialized((char *)name + (0 .. namelen - 1));
    assigns __fc_win_state \from __fc_win_state, s, ((char *)name)[0 .. namelen - 1];
    assigns \result \from __fc_win_state; */
int connect(SOCKET s, const struct sockaddr *name, int namelen);

/* Sends at most len bytes: returns how many, or SOCKET_ERROR. */
/*@ requires len: len >= 0;
    requires buf: \valid_read(buf + (0 .. len - 1)) && \initialized(buf + (0 .. len - 1));
    assigns __fc_win_state \from __fc_win_state, s, buf[0 .. len - 1];
    assigns \result \from __fc_win_state, len;
    ensures range: \result == SOCKET_ERROR || 0 <= \result <= len; */
int send(SOCKET s, const char *buf, int len, int flags);

/* Receives at most len bytes: returns how many (0 at the end), or
 * SOCKET_ERROR. */
/*@ requires len: len >= 0;
    requires buf: \valid(buf + (0 .. len - 1));
    assigns buf[0 .. len - 1] \from __fc_win_state, s;
    assigns __fc_win_state \from __fc_win_state, s;
    assigns \result \from __fc_win_state, len;
    ensures range: \result == SOCKET_ERROR || 0 <= \result <= len; */
int recv(SOCKET s, char *buf, int len, int flags);

/*@ requires src: valid_read_string(pszAddrString);
    requires af: Family == AF_INET || Family == AF_INET6;
    requires dst4: Family == AF_INET ==> \valid((struct in_addr *)pAddrBuf);
    requires dst6: Family == AF_INET6 ==> \valid((struct in6_addr *)pAddrBuf);
    assigns ((char *)pAddrBuf)[0 .. (Family == AF_INET ? 4 : 16) - 1] \from Family, pszAddrString[0 ..];
    assigns \result \from Family, pszAddrString[0 ..];
    ensures range: \result == 1 || \result == 0 || \result == -1; */
int inet_pton(int Family, const char *pszAddrString, void *pAddrBuf);

#endif
