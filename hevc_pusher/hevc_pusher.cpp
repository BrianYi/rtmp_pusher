// hevc_rtmp_test.cpp : This file contains the 'main' function. Program execution begins and ends there.
//
#include <winsock2.h>
#include <iostream>
#include <vector>
#include <thread>
#include <string>
#include <queue>
#include <mutex>
#include "Packet.h"
#include "Log.h"

extern "C" {
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
}

// win socket
#pragma comment(lib, "ws2_32.lib")
// rtmpdump
//#pragma comment(lib, "rtmp/librtmp.lib")
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

enum
{
	STREAMING_START,
	STREAMING_IN_PROGRESS,
	STREAMING_STOPPING,
	STREAMING_STOPPED
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
	std::string filePath;
};

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

		if ( waitTime > 0 ) Sleep( waitTime );
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
			while ( send_push_packet( pusher->conn, *ptrPkt ) <= 0 )
				;
			free_packet( ptrPkt );
		}

// #ifdef _DEBUG
// 			RTMP_Log( RTMP_LOGDEBUG, "throw out packet! packet timestamp=%lld, current timestamp=%lld, diff=%lld",
// 					  ptrPkt->header.timestamp, currentTime, ptrPkt->header.timestamp - currentTime );
// 
// 			RTMP_LogHexStr( RTMP_LOGDEBUG, ( uint8_t * ) ptrPkt, packSize );
// #endif // _DEBUG
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
		if ( send_createStream_packet( pusher->conn,
											 get_current_milli( ),
											 pusher->stream.app.c_str( ),
											 pusher->stream.timebase ) <= 0 )
		{
			Sleep( 100 );
			continue;
		}

		Sleep( 10 );	// wait for packet comming

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
		Sleep( 100 );
	}

	AVFormatContext *pFmtCtx = avformat_alloc_context( );
	if ( avformat_open_input( &pFmtCtx, pusher->filePath.c_str(), 
							  NULL, NULL ) != 0 )
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
	AVPacket pkt;		// ffmpeg packet
	av_init_packet( &pkt );
	PACKET* ptrPkt;	// my packet
	int time_interval = pusher->stream.timebase;
	int64_t lastSendTime = 0, currentTime = 0;
	int64_t nextSendTime = get_current_milli( );
	int64_t waitTime = 0;
	int32_t maxSendBuf = SEND_BUF_SIZE;
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

		int numPack = NUM_PACK( pkt.size );
		std::unique_lock<std::mutex> lock( pusher->mux );
		if ( maxSendBuf < pkt.size )
		{
			maxSendBuf = (pkt.size + MAX_PACKET_SIZE - 1) / MAX_PACKET_SIZE * MAX_PACKET_SIZE;
			pusher->conn.set_socket_sndbuf_size( maxSendBuf );
		}
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
	stream.streamData.push( ptrPkt );
	Sleep( 100 );
	stopStreaming( pusher );
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
	FILE* dumpfile = fopen( "hevc_pusher.dump", "a+" );
	RTMP_LogSetOutput( dumpfile );
	RTMP_LogSetLevel( RTMP_LOGALL );
	RTMP_LogThreadStart( );

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
	pusher->filePath = "E:\\Movie\\test video\\bbb_sunflower_1080p_60fps_normal.mp4";
	pusher->stream.timebase = 1000 / 40;
	while ( 0 != pusher->conn.connect_to( SERVER_IP, SERVER_PORT ) )
	{
		Sleep( 1000 );
		continue;
	}
#ifdef _DEBUG
	RTMP_Log( RTMP_LOGDEBUG, "connect to %s:%d success.",
			  SERVER_IP, SERVER_PORT );
#endif // _DEBUG
	std::thread sender( thread_func_for_sender, pusher );
	std::thread reader( thread_func_for_reader, pusher );
	std::thread aliver( thread_func_for_aliver, pusher );
	//std::thread controller( thread_func_for_controller, pusher );

	sender.join( );
	reader.join( );
	aliver.join( );
#ifdef _DEBUG
	RTMP_LogThreadStop( );
#endif // _DEBUG
	Sleep( 10 );

	if ( pusher )
		free( pusher );
#ifdef _DEBUG
	if ( dumpfile )
		fclose( dumpfile );
#endif
	cleanup_sockets( );
#ifdef _DEBUG
	_CrtDumpMemoryLeaks( );
#endif // _DEBUG
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
