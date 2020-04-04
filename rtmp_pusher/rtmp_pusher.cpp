/*
 * Copyright (C) 2020 BrianYi, All rights reserved
 */

#include <winsock2.h>
#include <iostream>
#include <vector>
#include <thread>
#include <string>
#include <queue>
#include <mutex>
//#define TIME_CACULATE
#include "Packet.h"
#include "Log.h"

// win socket
#pragma comment(lib, "ws2_32.lib")

//#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")

#define SERVER_IP "192.168.1.105"
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
	StatisticInfo stat;
	fd_set fdSet;
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

#ifdef _DEBUG
		TIME_BEG( 1 );
#endif // _DEBUG
		if ( waitTime > 0 ) Sleep( waitTime );
#ifdef _DEBUG
		TIME_END( 1 );
#endif

#ifdef _DEBUG
			TIME_BEG( 2 ); //1124ms 1235ms
#endif // _DEBUG
		timeval tm{0,100};
		fd_set fdSet = pusher->fdSet;
		while ( select( 0, nullptr, &fdSet, nullptr, &tm ) <= 0 && 
				pusher->state == STREAMING_START)
		{
			fdSet = pusher->fdSet;
			Sleep( 10 );
		};

		if ( send_push_packet( pusher->conn, *ptrPkt ) <= 0 )
		{
			RTMP_LogAndPrintf( RTMP_LOGDEBUG, "send push packet error %s:%d", __FUNCTION__, __LINE__ );
			break; // error
		}
		caculate_statistc( pusher->stat, *ptrPkt, StatSend );

		if ( ptrPkt->header.type == Fin )
		{
			stopStreaming( pusher );
			free_packet( ptrPkt );
			break;
		}
		free_packet( ptrPkt );
#ifdef _DEBUG
		TIME_END( 2 );
#endif // _DEBUG
	};
	RTMP_Log(RTMP_LOGDEBUG, "sender thread is quit." );
	return true;
}

int thread_func_for_reader( void *arg )
{
	RTMP_Log(RTMP_LOGDEBUG, "reader thread is start..." );
	STREAMING_PUSHER *pusher = ( STREAMING_PUSHER * ) arg;

	fd_set fdSet = pusher->fdSet;
	timeval tm{0,100}; // 设置超时时间
	int isAck = false;
	while ( !isAck )
	{
		while ( select( 0, nullptr, &fdSet, nullptr, &tm ) <= 0 )
		{
			fdSet = pusher->fdSet;
			Sleep( 10 );
		}
		send_createStream_packet( pusher->conn,
								  get_current_milli( ),
								  pusher->stream.app.c_str( ),
								  pusher->stream.timebase );
		// recv ack
		PACKET pkt;
		if ( recv_packet( pusher->conn, pkt ) <= 0 )
			continue;
		switch ( pkt.header.type )
		{
		case Ack:
			printf( "Begin to push stream.\n" );
			isAck = true;
			break;
		case Err:
			printf( "Already has a stream name is %s.\n", pkt.header.app );
			break;
		default:
			printf( "unknown packet.\n" );
		}
	}

// 	AVFormatContext *pFmtCtx = avformat_alloc_context( );
// 	if ( avformat_open_input( &pFmtCtx, pusher->filePath.c_str(), 
// 							  NULL, NULL ) != 0 )
// 		return 0;
// 
// 	if ( avformat_find_stream_info( pFmtCtx, NULL ) < 0 )
// 		return 0;
// 	AVStream *st = pFmtCtx->streams[ 0 ];
// 	AVCodec *pCodec = avcodec_find_decoder( st->codecpar->codec_id );
// 	AVCodecContext *pCodecCtx = avcodec_alloc_context3( pCodec );
// 	if ( !pCodecCtx )
// 		return -1;
// 	if ( !pCodec )
// 		return -1;
// 
// 	if ( avcodec_parameters_to_context( pCodecCtx, st->codecpar ) < 0 )
// 		return -1;
// 	if ( avcodec_open2( pCodecCtx, pCodec, NULL ) < 0 )
// 		return -1;

	StreamInfo& stream = pusher->stream;
	PACKET* ptrPkt;	// my packet
	int time_interval = pusher->stream.timebase;
	int64_t lastSendTime = 0, currentTime = 0;
	int64_t nextSendTime = get_current_milli( );
	int64_t waitTime = 0;
	int32_t maxSendBuf = SEND_BUF_SIZE;
// 	AVPacket packet;		// ffmpeg packet
// 	AVFrame frame;
	int response = 0;
	FILE *fd = fopen( pusher->filePath.c_str(), "rb" );
	if ( !fd )
	{
		RTMP_LogAndPrintf( RTMP_LOGDEBUG, "open file %s failed.", pusher->filePath.c_str( ) );
		stopStreaming( pusher );
		return -1;
	}
	char buf[ MAX_BODY_SIZE * 50 ];
	int32_t readBytes = 0;
	int64_t totalReadBytes = 0;
	while ( !feof(fd) )
	{
#ifdef _DEBUG
		TIME_BEG( 3 );
#endif // _DEBUG
		readBytes = fread( buf, sizeof *buf, sizeof buf, fd );
		totalReadBytes += readBytes;
#ifdef _DEBUG
		RTMP_Log( RTMP_LOGDEBUG, "readBytes=%d,totalReadBytes=%lld", readBytes, totalReadBytes );
#endif // _DEBUG
		currentTime = get_current_milli( );
		waitTime = nextSendTime - currentTime;
		if ( waitTime > 0 )
			Sleep( waitTime );
		else
			nextSendTime = currentTime;
		lastSendTime = nextSendTime;
		nextSendTime = lastSendTime + time_interval;
#ifdef _DEBUG
		TIME_END( 3 );
#endif // _DEBUG
		int numPack = NUM_PACK( readBytes );
		std::unique_lock<std::mutex> lock( pusher->mux );
#ifdef _DEBUG
		TIME_BEG( 4 );
#endif // _DEBUG
		if ( maxSendBuf < readBytes )
		{
			maxSendBuf = ( readBytes + MAX_PACKET_SIZE - 1 ) / MAX_PACKET_SIZE * MAX_PACKET_SIZE;
			pusher->conn.set_socket_sndbuf_size( maxSendBuf );
		}
		for ( int i = 0; i < numPack; ++i )
		{
			ptrPkt = alloc_push_packet( readBytes,
										i != numPack - 1,
										i * MAX_BODY_SIZE,
										nextSendTime,
										stream.app.c_str( ),
										( char * ) buf + i * MAX_BODY_SIZE );

			stream.streamData.push( ptrPkt );
		}
#ifdef _DEBUG
		RTMP_Log( RTMP_LOGDEBUG, "streamData.size==%d", stream.streamData.size( ) );
#endif // _DEBUG
#ifdef _DEBUG
		TIME_END( 4 );
#endif // _DEBUG
	}
	fclose( fd );
// 	while ( av_read_frame( pFmtCtx, ptrPacket ) >= 0 )
// 	{
// 		if ( ptrPacket->stream_index != st->index ) continue;
// 		if ( avcodec_send_packet( pCodecCtx, ptrPacket ) < 0 ) break;
// 		while ( response = avcodec_receive_frame( pCodecCtx, ptrFrame ) >= 0 )
// 		{
// 			if ( response == AVERROR( EAGAIN ) || response == AVERROR_EOF )
// 				break;
// 
// 
// 			currentTime = get_current_milli( );
// 			waitTime = nextSendTime - currentTime;
// 			if ( waitTime > 0 )
// 				Sleep( waitTime );
// 			else
// 				nextSendTime = currentTime;
// 			lastSendTime = nextSendTime;
// 			nextSendTime = lastSendTime + time_interval;
// 
// 			int numPack = NUM_PACK( ptrFrame->width );
// 			std::unique_lock<std::mutex> lock( pusher->mux );
// 			if ( maxSendBuf < pkt.size )
// 			{
// 				maxSendBuf = ( pkt.size + MAX_PACKET_SIZE - 1 ) / MAX_PACKET_SIZE * MAX_PACKET_SIZE;
// 				pusher->conn.set_socket_sndbuf_size( maxSendBuf );
// 			}
// 			for ( int i = 0; i < numPack; ++i )
// 			{
// 				ptrPkt = alloc_push_packet( pkt.size,
// 											i != numPack - 1,
// 											i * MAX_BODY_SIZE,
// 											nextSendTime,
// 											stream.app.c_str( ),
// 											( char * ) ptrFrame.data + i * MAX_BODY_SIZE );
// 
// 				stream.streamData.push( ptrPkt );
// 			}
// 		}
// 	}
	nextSendTime += time_interval;

	ptrPkt = alloc_fin_packet( nextSendTime, stream.app.c_str( ) );
	stream.streamData.push( ptrPkt );
	//Sleep( 100 );
	//stopStreaming( pusher );
// 	avformat_close_input( &pFmtCtx );
// 	avformat_free_context( pFmtCtx );
	RTMP_Log(RTMP_LOGDEBUG, "reader thread is quit." );
	return true;
}

void show_statistics( STREAMING_PUSHER* pusher )
{
	printf( "%-15s%-6s%-8s%-20s %-8s\t\t%-13s\t%-10s\t%-15s\t %-8s\t%-13s\t%-10s\t%-15s\n",
			"ip", "port", "type", "app",
			"rec-byte", "rec-byte-rate", "rec-packet", "rec-packet-rate",
			"snd-byte", "snd-byte-rate", "snd-packet", "snd-packet-rate" );


	printf( "%-15s%-6d%-8s%-20s %-6.2fMB\t\t%-9.2fKB/s\t%-10lld\t%-13lld/s\t %-6.2fMB\t%-9.2fKB/s\t%-10lld\t%-13lld/s\n",
			pusher->conn.getIP( ).c_str( ),
			pusher->conn.getPort( ),
			"Pusher",
			pusher->stream.app.c_str(),

			MB( pusher->stat.recvBytes ),
			KB( pusher->stat.recvByteRate ),
			pusher->stat.recvPackets,
			pusher->stat.recvPacketRate,

			MB( pusher->stat.sendBytes ),
			KB( pusher->stat.sendByteRate ),
			pusher->stat.sendPackets,
			pusher->stat.sendPacketRate );
}

int thread_func_for_controller( void *arg )
{
	RTMP_Log(RTMP_LOGDEBUG, "controller thread is start..." );
	STREAMING_PUSHER *pusher = ( STREAMING_PUSHER * ) arg;
	while ( pusher->state == STREAMING_START )
	{
		system( "cls" );
		show_statistics( pusher );
		Sleep( 1000 );
// 		ich = getchar( );
// 		switch ( ich )
// 		{
// 		case 'q':
// 			RTMP_Log(RTMP_LOGDEBUG, "Exiting" );
// 			stopStreaming( pusher );
// 			break;
// 		default:
// 			RTMP_Log(RTMP_LOGDEBUG, "Unknown command \'%c\', ignoring", ich );
// 		}
	}
	RTMP_Log(RTMP_LOGDEBUG, "controller thread is quit." );
	return true;
}

int main( int argc, char* argv[] )
{
	if ( argc < 3 )
	{
		printf( "please pass in live name and file path parameter.\n" );
		printf( "usage: pusher \"live-name\" \"/path/to/file\" \n" );
		return 0;
	}
	FILE* dumpfile = nullptr;
	if ( argv[ 3 ] )
		dumpfile = fopen( argv[ 3 ], "a+" );
	else
		dumpfile = fopen( "rtmp_pusher.dump", "a+" );
	RTMP_LogSetOutput( dumpfile );
	RTMP_LogSetLevel( RTMP_LOGALL );
	RTMP_LogThreadStart( );

	SYSTEMTIME tm;
	GetSystemTime( &tm );
	RTMP_Log( RTMP_LOGDEBUG, "==============================" );
	RTMP_Log( RTMP_LOGDEBUG, "log file:\trtmp_pusher.dump" );
	RTMP_Log( RTMP_LOGDEBUG, "log timestamp:\t%lld", get_current_milli( ) );
	RTMP_Log( RTMP_LOGDEBUG, "log date:\t%d-%d-%d %d:%d:%d.%d",
			  tm.wYear,
			  tm.wMonth,
			  tm.wDay,
			  tm.wHour + 8, tm.wMinute, tm.wSecond, tm.wMilliseconds );
	RTMP_Log( RTMP_LOGDEBUG, "==============================" );
	init_sockets( );

	STREAMING_PUSHER *pusher = new STREAMING_PUSHER;
	pusher->state = STREAMING_START;
	pusher->stream.app = argv[1]; // "live"
	pusher->filePath = argv[2]; // "E:\\Movie\\test video\\small_bunny_1080p_60fps.mp4"
	pusher->stream.timebase = 1000 / 25;
	ZeroMemory( &pusher->stat, sizeof StatisticInfo );
	FD_ZERO( &pusher->fdSet );
	FD_SET( pusher->conn.m_socketID, &pusher->fdSet );
	while ( 0 != pusher->conn.connect_to( SERVER_IP, SERVER_PORT ) )
	{
		printf( "Connect to server %s:%d failed.\n", SERVER_IP, SERVER_PORT );
		Sleep( 1000 );
		continue;
	}
	RTMP_Log( RTMP_LOGDEBUG, "connect to %s:%d success.",
			  SERVER_IP, SERVER_PORT );
	std::thread sender( thread_func_for_sender, pusher );
	std::thread reader( thread_func_for_reader, pusher );
	std::thread controller( thread_func_for_controller, pusher );

	sender.join( );
	reader.join( );
	controller.join( );
	RTMP_LogThreadStop( );

	Sleep( 10 );

	if ( pusher )
		free( pusher );
	if ( dumpfile )
		fclose( dumpfile );
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
