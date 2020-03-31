// hevc_rtmp_test.cpp : This file contains the 'main' function. Program execution begins and ends there.
//
#include <winsock2.h>
#include <iostream>
#include <vector>
#include <thread>
#include <string>
#include <queue>
#include <mutex>
#include "TCP.h"
#include "librtmp/log.h"
extern "C" {
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
}

// win socket
#pragma comment(lib, "ws2_32.lib")
// rtmpdump
#pragma comment(lib, "rtmp/librtmp.lib")
// openssl
#pragma comment(lib, "openssl/libeay32.lib")
#pragma comment(lib, "openssl/ssleay32.lib")
// ffmpeg
#pragma comment(lib, "ffmpeg/avformat.lib")
#pragma comment(lib, "ffmpeg/avcodec.lib")
#pragma comment(lib, "ffmpeg/avutil.lib")

#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")

#define STREAM_CHANNEL_METADATA  0x03
#define STREAM_CHANNEL_VIDEO     0x04
#define STREAM_CHANNEL_AUDIO     0x05

#define SERVER_IP "192.168.1.104"
#define SERVER_PORT 5566
#define MAX_BODY_SIZE 1400
#define MAX_PACKET_SIZE (MAX_BODY_SIZE+sizeof HEADER)

#define BODY_SIZE(MP,size,seq)	(MP?MAX_BODY_SIZE:size - seq)
#define BODY_SIZE_H(header)		BODY_SIZE(header.MP,header.size,header.seq)
#define PACK_SIZE(MP,size,seq)	(MP?MAX_PACKET_SIZE:(sizeof HEADER+BODY_SIZE(MP,size,seq)))
#define PACK_SIZE_H(header)		PACK_SIZE(header.MP,header.size,header.seq)
#define INVALID_PACK(pack)		((pack.header.type < 0 || pack.header.type >= TypeNum) || (PACK_SIZE_H(pack.header) > MAX_PACKET_SIZE))

enum
{
	STREAMING_START,
	STREAMING_IN_PROGRESS,
	STREAMING_STOPPING,
	STREAMING_STOPPED
};

enum
{
	CreateStream,
	Play,
	Push,
	Pull,
	Ack,
	Alive,
	Fin,
	Err,
	TypeNum
};
// 4+4+4+8+8+16=44
#pragma pack(1)
struct HEADER
{
	// total body size
	int32_t size;
	int32_t type;			// setup(0),push(1),pull(2),ack(3),err(4)
	// 
	// default 0 
	// setup: timebase=1000/fps
	// push,pull: more fragment
	// 
	int32_t reserved;		
	int32_t MP;				// more packet?
	int32_t seq;			// sequence number
	int64_t timestamp;		// send time
	char app[ 16 ];		// app
};
#pragma pack()

struct PACKET
{
	HEADER header;
	char body[MAX_BODY_SIZE];
};


typedef std::queue<PACKET*> StreamData;
struct StreamInfo
{
	std::string app;
	int timebase;
	StreamData streamData;
};

struct STREAMING_PUSHER
{
	TCP conn;
	int state;
	StreamInfo stream;
	std::mutex mux;
};

inline long long get_current_milli( )
{
	return std::chrono::duration_cast< std::chrono::milliseconds >
		( std::chrono::system_clock::now( ).time_since_epoch( ) ).count( );
}

bool init_sockets( )
{
#ifdef WIN32
	WORD version = MAKEWORD( 1, 1 );
	WSADATA wsaData;
	return ( WSAStartup( version, &wsaData ) == 0 );
#endif
	return true;
}

void cleanup_sockets( )
{
#ifdef WIN32
	WSACleanup( );
#endif
}

#define MAX_LOG_SIZE 2048
struct LOG
{
	size_t dataSize;
	char* data;
};
std::queue<LOG> logQue;
FILE *dumpfile;
std::mutex mux;

static void logCallback( int level, const char *format, va_list vl )
{
	char str[ MAX_LOG_SIZE ] = "";
	const char *levels[ ] = {
	  "CRIT", "ERROR", "WARNING", "INFO",
	  "DEBUG", "DEBUG2"
	};
	vsnprintf( str, MAX_LOG_SIZE - 1, format, vl );

	/* Filter out 'no-name' */
	if ( RTMP_debuglevel < RTMP_LOGALL && strstr( str, "no-name" ) != NULL )
		return;

	if ( level <= RTMP_debuglevel )
	{
		LOG log;
		log.data = ( char * ) malloc( MAX_LOG_SIZE );
		log.dataSize = sprintf( log.data, "%s: %s", levels[ level ], str );
		std::unique_lock<std::mutex> lock( mux );
		logQue.push( log );
		//fprintf( fmsg, "%s: %s\n", levels[ level ], str );
	}
}

void RTMP_LogHexStr( int level, const uint8_t *data, unsigned long len )
{
#define BP_OFFSET 9
#define BP_GRAPH 60
#define BP_LEN	80
	static const char hexdig[ ] = "0123456789abcdef";
	char	line[ BP_LEN ];
	unsigned long i;
	const char *levels[ ] = {
	  "CRIT", "ERROR", "WARNING", "INFO",
	  "DEBUG", "DEBUG2"
	};
	if ( !data || level > RTMP_debuglevel )
		return;

	/* in case len is zero */
	line[ 0 ] = '\0';
	std::string tmpStr;
	for ( i = 0; i < len; i++ )
	{
		int n = i % 16;
		unsigned off;

		if ( !n )
		{
			if ( i )
			{
				tmpStr = tmpStr + levels[ level ] + ": " + line + '\n';
			}
			memset( line, ' ', sizeof( line ) - 2 );
			line[ sizeof( line ) - 2 ] = '\0';

			off = i % 0x0ffffU;

			line[ 2 ] = hexdig[ 0x0f & ( off >> 12 ) ];
			line[ 3 ] = hexdig[ 0x0f & ( off >> 8 ) ];
			line[ 4 ] = hexdig[ 0x0f & ( off >> 4 ) ];
			line[ 5 ] = hexdig[ 0x0f & off ];
			line[ 6 ] = ':';
		}

		off = BP_OFFSET + n * 3 + ( ( n >= 8 ) ? 1 : 0 );
		line[ off ] = hexdig[ 0x0f & ( data[ i ] >> 4 ) ];
		line[ off + 1 ] = hexdig[ 0x0f & data[ i ] ];

		off = BP_GRAPH + n + ( ( n >= 8 ) ? 1 : 0 );

		if ( isprint( data[ i ] ) )
		{
			line[ BP_GRAPH + n ] = data[ i ];
		}
		else
		{
			line[ BP_GRAPH + n ] = '.';
		}
	}
	tmpStr = tmpStr + levels[ level ] + ": " + line;
	
	LOG log;
	log.data = ( char* ) malloc( tmpStr.size( ) + 1 );
	log.dataSize = tmpStr.size( );
	memcpy( log.data, tmpStr.c_str( ), log.dataSize );
	log.data[ log.dataSize ] = '\0';
	std::unique_lock<std::mutex> lock( mux );
	logQue.push( log );
}

int thread_func_for_logger( void *arg )
{
	RTMP_Log( RTMP_LOGDEBUG, "logger thread is start..." );
	STREAMING_PUSHER *pusher = ( STREAMING_PUSHER * ) arg;

	while ( pusher->state == STREAMING_START )
	{
		if ( logQue.empty( ) )
		{
			Sleep( 10 );
			continue;
		}
		std::unique_lock<std::mutex> lock( mux );
		LOG log = logQue.front( );
		logQue.pop( );
		lock.unlock( );
		fprintf( dumpfile, "%s\n", log.data );
#ifdef _DEBUG
		fflush( dumpfile );
#endif
		free( log.data );
		Sleep( 10 );
	}

	RTMP_Log( RTMP_LOGDEBUG, "logger thread is quit." );
	return true;
}


void
stopStreaming( STREAMING_PUSHER * pusher )
{
	if ( pusher->state != STREAMING_STOPPED )
	{
		if ( pusher->state == STREAMING_IN_PROGRESS )
		{
			pusher->state = STREAMING_STOPPING;

			// wait for streaming threads to exit
			while ( pusher->state != STREAMING_STOPPED )
				Sleep( 10 );
		}
		pusher->state = STREAMING_STOPPED;
	}
}

void zero_packet( PACKET& pkt )
{
	memset( &pkt, 0, sizeof PACKET );
}

void free_packet( PACKET* ptrPkt )
{
	if ( ptrPkt )
		free( ptrPkt );
}


int send_pkt( TCP& conn, size_t size, int type, int reserved, int MP,
					 int32_t seq, int64_t timestamp, const char* app, const char *body )
{
	int bodySize = BODY_SIZE( MP, size, seq );
	int packSize = PACK_SIZE( MP, size, seq );
#ifdef _DEBUG
	if ( type != Alive )
	{
		int64_t sendTimestamp = get_current_milli( );
		RTMP_Log( RTMP_LOGDEBUG, "send packet(%d) to %s:%u, %dB:[%d,%d-%d], packet timestamp=%lld, send timestamp=%lld, S-P=%lld",
				  type,
				  conn.getIP( ).c_str( ),
				  conn.getPort( ),
				  MAX_PACKET_SIZE,
				  size,
				  seq,
				  seq + bodySize,
				  timestamp,
				  sendTimestamp,
				  sendTimestamp - timestamp );
	}
#endif // _DEBUG

	PACKET pkt;
	pkt.header.size = htonl( size );
	pkt.header.type = htonl( type );
	pkt.header.reserved = htonl( reserved );
	pkt.header.MP = htonl( MP );
	pkt.header.seq = htonl( seq );
	pkt.header.timestamp = htonll( timestamp );
	strcpy( pkt.header.app, app );
	if ( bodySize > 0 )
		memcpy( pkt.body, body, bodySize );
#ifdef _DEBUG
	if ( type != Alive )
		RTMP_LogHexStr( RTMP_LOGDEBUG, ( uint8_t * ) &pkt, packSize );
#endif // _DEBUG
	return conn.send( ( char * ) &pkt, MAX_PACKET_SIZE );
}


int recv_packet( TCP& conn, PACKET& pkt, IOType inIOType = Blocking )
{
	int recvSize = conn.receive( (char *)&pkt, MAX_PACKET_SIZE, inIOType );
	if ( recvSize <= 0 )
	{
		//RTMP_Log(RTMP_LOGDEBUG, "recv size is <= 0 %d %s", __LINE__, __FUNCTION__ );
		return recvSize;
	}

	size_t bodySize = BODY_SIZE( ntohl( pkt.header.MP ),
								 ntohl( pkt.header.size ),
								 ntohl( pkt.header.seq ) );
	size_t packSize = PACK_SIZE( ntohl( pkt.header.MP ),
								 ntohl( pkt.header.size ),
								 ntohl( pkt.header.seq ) );
#ifdef _DEBUG
	if ( ntohl( pkt.header.type) != Alive )
	{
		int64_t recvTimestamp = get_current_milli( );
		RTMP_Log( RTMP_LOGDEBUG, "recv packet(%d) from %s:%u, %dB:[%d,%d-%d], packet timestamp=%lld, recv timestamp=%lld, R-P=%lld",
				  ntohl( pkt.header.type),
				  conn.getIP( ).c_str( ),
				  conn.getPort( ),
				  MAX_PACKET_SIZE,
				  ntohl( pkt.header.size),
				  ntohl( pkt.header.seq),
				  ntohl( pkt.header.seq) + bodySize,
				  ntohll( pkt.header.timestamp),
				  recvTimestamp,
				  recvTimestamp - ntohll( pkt.header.timestamp) );
		RTMP_LogHexStr( RTMP_LOGDEBUG, ( uint8_t * ) &pkt, packSize );
	}
#endif // _DEBUG
	
	pkt.header.size = ntohl( pkt.header.size );
	pkt.header.type = ntohl( pkt.header.type );
	pkt.header.reserved = ntohl( pkt.header.reserved );
	pkt.header.MP = ntohl( pkt.header.MP );
	pkt.header.seq = ntohl( pkt.header.seq );
	pkt.header.timestamp = ntohll( pkt.header.timestamp );
	return recvSize;
}


#define send_createStream_packet(conn, timestamp, app, timebase) \
	send_pkt( conn, 0, CreateStream, timebase,0,  0, timestamp, app, nullptr )
#define send_play_packet(conn, timestamp, app ) \
	send_pkt( conn, 0, Play, 0, 0, 0, timestamp, app, nullptr )
#define send_ack_packet(conn, timestamp, app, timebase ) \
	send_pkt( conn, 0, Ack, timebase,0,  0, timestamp, app, nullptr )
#define send_alive_packet(conn, timestamp, app ) \
	send_pkt( conn, 0, Alive, 0, 0, 0, timestamp, app, nullptr )
#define send_fin_packet(conn, timestamp, app ) \
	send_pkt( conn, 0, Fin,0, 0, 0, timestamp, app, nullptr )
#define send_err_packet(conn, timestamp, app ) \
	send_pkt( conn, 0, Err,0, 0, 0, timestamp, app, nullptr )

#define send_push_packet(conn, pkt) \
	send_packet( conn, pkt )
#define send_pull_packet(conn, pkt) \
	send_packet( conn, pkt )

#define alloc_createStream_packet(timestamp, app, timebase) \
	alloc_packet(0, CreateStream, timebase, 0, 0, timestamp, app, nullptr )
#define alloc_play_packet(timestamp, app ) \
	alloc_packet( 0, Play,0,  0, 0, timestamp, app, nullptr )
#define alloc_ack_packet(timestamp, app ) \
	alloc_packet( 0, Ack,0,  0, 0, timestamp, app, nullptr )
#define alloc_alive_packet(timestamp, app ) \
	alloc_packet( 0, Alive,0,  0, 0, timestamp, app, nullptr )
#define alloc_err_packet(timestamp, app ) \
	alloc_packet( 0, Err,0,  0, 0, timestamp, app, nullptr )
#define alloc_fin_packet(timestamp, app) \
	alloc_packet(0, Fin,0,  0, 0, timestamp, app, nullptr)

#define alloc_push_packet(size, MP, seq, timestamp, app, body ) \
	alloc_packet(size, Push, 0, MP, seq, timestamp, app, body )
#define alloc_pull_packet(size, MP, seq, timestamp, app, body ) \
	alloc_packet(size, Pull, 0, MP, seq,  timestamp, app, body )


PACKET* alloc_packet( size_t size, int type, int reserved, int MP,
							int32_t seq, int64_t timestamp,
							const char* app, const char *body )
{
	size_t bodySize = BODY_SIZE( MP,size,seq );
	PACKET* pkt = ( PACKET* ) malloc( sizeof PACKET );
	zero_packet( *pkt );
	pkt->header.size = size;			// packet size
	pkt->header.type = type;			// setup(0),push(1),pull(2),ack(3),err(4)
	pkt->header.reserved = reserved;		// default 0, setup:timebase=1000/fps
	pkt->header.MP = MP;
	pkt->header.seq = seq;			// sequence number
	pkt->header.timestamp = timestamp;		// send time
	strcpy( pkt->header.app, app );
	if ( bodySize > 0 )
	{
		memcpy( pkt->body, body, bodySize );
	}
	return pkt;
}


int send_packet( TCP& conn, PACKET pkt )
{
	return send_pkt( conn, pkt.header.size, pkt.header.type,
					 pkt.header.reserved, pkt.header.MP, pkt.header.seq,
					 pkt.header.timestamp, pkt.header.app, pkt.body );
}

int thread_func_for_sender( void *arg )
{
	RTMP_Log(RTMP_LOGDEBUG, "sender thread is start..." );
	STREAMING_PUSHER *pusher = ( STREAMING_PUSHER * ) arg;
	
	// begin push
	// read one nalu
	// maybe do while more better
	StreamData& streamData = pusher->stream.streamData;
	int64_t currentTime = 0, waitTime = 0;
	size_t bodySize, packSize;
	while ( pusher->state == STREAMING_START )
	{
		if ( streamData.empty( ) )
		{
			Sleep( 5 );
			continue;
		}

		std::unique_lock<std::mutex> lock( pusher->mux );
		PACKET* ptrPkt = streamData.front( );
		streamData.pop( );
		lock.unlock( );

		currentTime = get_current_milli( );
		waitTime = ptrPkt->header.timestamp - currentTime;
		bodySize = BODY_SIZE_H( ptrPkt->header );
		packSize = PACK_SIZE_H( ptrPkt->header );
		if ( waitTime >= 0 )
		{
			if (waitTime > 0) Sleep( waitTime );
			int MP = ptrPkt->header.MP;
			send_push_packet( pusher->conn, *ptrPkt );
			free_packet( ptrPkt );
			while ( MP )
			{
				lock.lock( );
				ptrPkt = streamData.front( );
				streamData.pop( );
				lock.unlock( );
				MP = ptrPkt->header.MP;
				send_push_packet( pusher->conn, *ptrPkt );
				free_packet( ptrPkt );
			}
		}
		else
		{
#ifdef _DEBUG
			RTMP_Log( RTMP_LOGDEBUG, "throw out packet! packet timestamp=%lld, current timestamp=%lld, diff=%lld",
					  ptrPkt->header.timestamp, currentTime, ptrPkt->header.timestamp - currentTime );

			RTMP_LogHexStr( RTMP_LOGDEBUG, ( uint8_t * ) ptrPkt, packSize );
#endif // _DEBUG
			free_packet( ptrPkt );
		}
	};
	RTMP_Log(RTMP_LOGDEBUG, "sender thread is quit." );
	return true;
}

int thread_func_for_reader( void *arg )
{
	RTMP_Log(RTMP_LOGDEBUG, "reader thread is start..." );
	STREAMING_PUSHER *pusher = ( STREAMING_PUSHER * ) arg;

	while ( true )
	{
		int ret = 0;
		if ( (ret = send_createStream_packet( pusher->conn,
											 get_current_milli( ),
											 pusher->stream.app.c_str( ),
											 pusher->stream.timebase )) <= 0 )
		{
			Sleep( 10 );
			continue;
		}

		// recv ack
		PACKET pkt;
		if ( recv_packet( pusher->conn, pkt, NonBlocking ) <= 0)
		{
			Sleep( 10 );
			continue;
		}

		if ( pkt.header.type == Ack )
		{
			break;
		}
		Sleep( 10 );
	}

	//Sleep( INFINITE );
	AVFormatContext *pFmtCtx = avformat_alloc_context( );
	if ( avformat_open_input( &pFmtCtx, "cuc_ieschool.h264", NULL, NULL ) != 0 )
		return 0;

	if ( avformat_find_stream_info( pFmtCtx, NULL ) < 0 )
		return 0;
	AVStream *st = pFmtCtx->streams[ 0 ];
	AVCodec *pCodec = avcodec_find_decoder( st->codecpar->codec_id );
	AVCodecContext *pCodecCtx = avcodec_alloc_context3( pCodec );
	if ( !pCodecCtx )
		return -1;
	if ( !pCodec )
		return -1;

	if ( avcodec_parameters_to_context( pCodecCtx, st->codecpar ) < 0 )
		return -1;
	if ( avcodec_open2( pCodecCtx, pCodec, NULL ) < 0 )
		return -1;

	StreamInfo& stream = pusher->stream;
	//while ( pusher->state == STREAMING_START )
	//{
	AVPacket pkt;		// ffmpeg packet
	av_init_packet( &pkt );
	PACKET* ptrPkt;	// my packet
	int time_interval = pusher->stream.timebase;
	int64_t lastSendTime = 0, currentTime = 0;
	int64_t nextSendTime = get_current_milli( );
	int64_t waitTime = 0;
	while ( av_read_frame( pFmtCtx, &pkt ) >= 0 )
	{
		currentTime = get_current_milli( );
		waitTime = nextSendTime - currentTime;
		if ( waitTime > 0 )
			Sleep( waitTime );
		else
			nextSendTime = currentTime; 
		lastSendTime = nextSendTime;
		nextSendTime = lastSendTime + time_interval;

		int numPack = ( pkt.size + MAX_BODY_SIZE - 1 ) / MAX_BODY_SIZE;

		std::unique_lock<std::mutex> lock( pusher->mux );
		for ( int i = 0; i < numPack; ++i )
		{
			ptrPkt = alloc_push_packet( pkt.size,
										i != numPack-1,
										i * MAX_BODY_SIZE,
										nextSendTime,
										stream.app.c_str( ),
										( char * ) pkt.data + i * MAX_BODY_SIZE );

			stream.streamData.push( ptrPkt );
		}
	}
	nextSendTime += time_interval;

	ptrPkt = alloc_fin_packet( nextSendTime, stream.app.c_str( ) );
// 
// 	zero_packet( pktStream );
// 	strcpy( pktStream.header.app, stream.app.c_str() );
// 	pktStream.header.reserved = 0;
// 	pktStream.header.seq = 0;
// 	pktStream.header.size = sizeof HEADER;
// 	pktStream.header.timestamp = nextSendTime + 40;
// 	pktStream.header.type = Fin;
	stream.streamData.push( ptrPkt );
	Sleep( 100 );
	stopStreaming( pusher );
	//}
	avformat_close_input( &pFmtCtx );
	avformat_free_context( pFmtCtx );
	RTMP_Log(RTMP_LOGDEBUG, "reader thread is quit." );
	return true;
}

int thread_func_for_controller( void *arg )
{
	RTMP_Log(RTMP_LOGDEBUG, "controller thread is start..." );
	STREAMING_PUSHER *pusher = ( STREAMING_PUSHER * ) arg;
	char ich;
	while ( pusher->state == STREAMING_START )
	{
		ich = getchar( );
		switch ( ich )
		{
		case 'q':
			RTMP_Log(RTMP_LOGDEBUG, "Exiting" );
			stopStreaming( pusher );
			break;
		default:
			RTMP_Log(RTMP_LOGDEBUG, "Unknown command \'%c\', ignoring", ich );
		}
	}
	RTMP_Log(RTMP_LOGDEBUG, "controller thread is quit." );
	return true;
}

int thread_func_for_aliver( void *arg )
{
	RTMP_Log( RTMP_LOGDEBUG, "cleaner thread is start..." );
	STREAMING_PUSHER* pusher = ( STREAMING_PUSHER* ) arg;
	StreamInfo& streamInfo = pusher->stream;
	while ( pusher->state == STREAMING_START )
	{
		// deal with temp connections
		// send heart packet
		std::unique_lock<std::mutex> lock( pusher->mux );
		send_alive_packet( pusher->conn,
						   get_current_milli( ),
						   streamInfo.app.c_str( ) );
		lock.unlock( );

		Sleep( 1000 );
	}
	RTMP_Log( RTMP_LOGDEBUG, "cleaner thread is quit." );
	return true;
}

int main( )
{
#ifdef _DEBUG
	dumpfile = fopen( "hevc_pusher.dump", "a+" );
	RTMP_LogSetOutput( dumpfile );
	RTMP_LogSetCallback( logCallback );
	RTMP_LogSetLevel( RTMP_LOGALL );

	SYSTEMTIME tm;
	GetSystemTime( &tm );
	RTMP_Log( RTMP_LOGDEBUG, "==============================" );
	RTMP_Log( RTMP_LOGDEBUG, "log file:\thevc_pusher.dump" );
	RTMP_Log( RTMP_LOGDEBUG, "log timestamp:\t%lld", get_current_milli( ) );
	RTMP_Log( RTMP_LOGDEBUG, "log date:\t%d-%d-%d %d:%d:%d.%d",
			  tm.wYear,
			  tm.wMonth,
			  tm.wDay,
			  tm.wHour + 8, tm.wMinute, tm.wSecond, tm.wMilliseconds );
	RTMP_Log( RTMP_LOGDEBUG, "==============================" );
#endif
	init_sockets( );

	STREAMING_PUSHER *pusher = new STREAMING_PUSHER;
	pusher->state = STREAMING_START;
	pusher->stream.app = "live";
	pusher->stream.timebase = 1000 / 25;
	while ( 0 != pusher->conn.connect_to( SERVER_IP, SERVER_PORT ) )
	{
		Sleep( 1000 );
		continue;
	}
#ifdef _DEBUG
	RTMP_Log( RTMP_LOGDEBUG, "connect to %s:%d success.",
			  SERVER_IP, SERVER_PORT );
#endif // _DEBUG
	std::thread logger( thread_func_for_logger, pusher );
	Sleep( 100 );
	std::thread sender( thread_func_for_sender, pusher );
	std::thread reader( thread_func_for_reader, pusher );
	std::thread aliver( thread_func_for_aliver, pusher );
	//std::thread controller( thread_func_for_controller, pusher );

	logger.join( );
	sender.join( );
	reader.join( );
	aliver.join( );

	if ( pusher )
		free( pusher );
#ifdef _DEBUG
	if ( dumpfile )
		fclose( dumpfile );
#endif
	cleanup_sockets( );
	return 0;
}

// Run program: Ctrl + F5 or Debug > Start Without Debugging menu
// Debug program: F5 or Debug > Start Debugging menu

// Tips for Getting Started: 
//   1. Use the Solution Explorer window to add/manage files
//   2. Use the Team Explorer window to connect to source control
//   3. Use the Output window to see build output and other messages
//   4. Use the Error List window to view errors
//   5. Go to Project > Add New Item to create new code files, or Project > Add Existing Item to add existing code files to the project
//   6. In the future, to open this project again, go to File > Open > Project and select the .sln file
