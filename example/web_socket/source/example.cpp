/*
 * Example illustrating web socket server.
 *
 * Server Usage:
 *    ./distribution/example/web_socket
 *
 * Client Usage:
 *    curl -w'\n' -v -X GET 'http://localhost:1984/socket'
 *
 * Further Reading:
 *
 */

#include <map>
#include <chrono>
#include <string>
#include <cstring>
#include <memory>
#include <utility>
#include <cstdlib>
#include <restbed>
#include <system_error>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

using namespace std;
using namespace restbed;
using namespace std::chrono;

shared_ptr< Service > service = nullptr;

map< string, shared_ptr< WebSocket > > sockets = { };

string base64_encode( const unsigned char* input, int length )
{
    BIO* bmem, *b64;
    BUF_MEM* bptr;
    
    b64 = BIO_new( BIO_f_base64( ) );
    bmem = BIO_new( BIO_s_mem( ) );
    b64 = BIO_push( b64, bmem );
    BIO_write( b64, input, length );
    BIO_flush( b64 );
    BIO_get_mem_ptr( b64, &bptr );
    
    char* buff = ( char* )malloc( bptr->length );
    memcpy( buff, bptr->data, bptr->length - 1 );
    buff[ bptr->length - 1 ] = 0;
    
    BIO_free_all( b64 );
    
    return buff;
}

multimap< string, string > build_websocket_handshake_response_headers( const shared_ptr< const Request >& request )
{
    auto key = request->get_header( "Sec-WebSocket-Key" );
    key.append( "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" );
    
    Byte hash[ SHA_DIGEST_LENGTH ];
    SHA1( reinterpret_cast< const unsigned char* >( key.data( ) ), key.length( ), hash );
    
    multimap< string, string > headers;
    headers.insert( make_pair( "Upgrade", "websocket" ) );
    headers.insert( make_pair( "Connection", "Upgrade" ) );
    headers.insert( make_pair( "Sec-WebSocket-Accept", base64_encode( hash, SHA_DIGEST_LENGTH ) ) );
    
    return headers;
}

void close_handler( const shared_ptr< WebSocket > socket )
{
    const auto key = socket->get_key( );
    sockets.erase( key );
    
    fprintf( stderr, "Closed connection to %s.\n", key.data( ) );
}

void error_handler( const shared_ptr< WebSocket > socket, const error_code error )
{
    const auto key = socket->get_key( );
    fprintf( stderr, "WebSocket Errored '%s' for %s.\n", error.message( ).data( ), key.data( ) );
}

void ping_handler( void )
{
    for ( auto entry : sockets )
    {
        auto key = entry.first;
        auto socket = entry.second;
        
        if ( socket->is_open( ) )
        {
            socket->send( WebSocketMessage::PING_FRAME );
        }
        else
        {
            socket->close( );
            sockets.erase( key );
        }
    }
    
    service->schedule( ping_handler, milliseconds( 5000 ) );
}

void message_handler( const shared_ptr< WebSocket > source, const shared_ptr< WebSocketMessage > message )
{
    const auto opcode = message->get_opcode( );
    
    if ( opcode == WebSocketMessage::PING_FRAME )
    {
        source->send( WebSocketMessage::PONG_FRAME );
    }
    else if ( opcode == WebSocketMessage::PONG_FRAME )
    {
        //Ignore PONG_FRAME.
        //
        //Every time the ping_handler is scheduled to run, it fires off a PING_FRAME to each
        //WebSocket. The client, if behaving correctly, will respond with a PONG_FRAME.
        //
        //On each occasion the underlying TCP socket sees any packet data transfer, whether
        //a PING, PONG, TEXT, or BINARY... frame. It will automatically reset the timeout counter
        //leaving the connection active; see also Settings::set_connection_timeout.
    }
    else if ( opcode == WebSocketMessage::CONNECTION_CLOSE_FRAME )
    {
        close_handler( source ); //source->close( ) which then invokes close_handler.
    }
    else
    {
        const auto source_key = source->get_key( );
        
        //remove from example, adds clutter.
        const auto data = String::format( "Received message '%.*s' from %s\n", message->get_data( ).size( ), message->get_data( ).data( ), source_key.data( ) );
        fprintf( stderr, "%s", data.data( ) );
        
        for ( auto socket : sockets )
        {
            auto destination_key = socket.first;
            
            if ( source_key not_eq destination_key )
            {
                auto destination = socket.second;
                destination->send( message );
            }
            
        }
        
        // auto flags = message->get_reserved_flags( );
        // fprintf( stderr, "Final Frame Flag: %d\n", message->get_final_frame_flag( ) );
        // fprintf( stderr, "Reserved Flags: %d %d %d\n", std::get<0>( flags ), std::get<1>( flags ), std::get<2>( flags ) );
        // fprintf( stderr, "OpCode: %d\n", message->get_opcode( ) );
        // fprintf( stderr, "Mask Flag %d\n", message->get_mask_flag( ) );
        // fprintf( stderr, "Payload Length %d\n", message->get_payload_length( ) );
        // fprintf( stderr, "Masking Key %u\n", message->get_mask( ) );
    }
}

void get_method_handler( const shared_ptr< Session > session )
{
    const auto request = session->get_request( );
    
    //if ( request->get_header( "connection", String::lowercase ) == "upgrade" ) //keep-alive, upgrade
    //{
    //if ( request->get_header( "upgrade", String::lowercase ) == "websocket" )
    //{
    const auto headers = build_websocket_handshake_response_headers( request );
    
    session->upgrade( SWITCHING_PROTOCOLS, headers, [ ]( const shared_ptr< WebSocket > socket )
    {
        if ( socket->is_open( ) )
        {
            socket->set_close_handler( close_handler ); //when is this invoked?
            socket->set_error_handler( error_handler );
            socket->set_message_handler( message_handler );
            
            socket->send( "Welcome to Corvusoft Chat!", [ ]( const shared_ptr< WebSocket > socket )
            {
                const auto key = socket->get_key( );
                sockets.insert( make_pair( key, socket ) );
                
                fprintf( stderr, "Sent welcome message to %s.\n", key.data( ) );
            } );
        }
        else
        {
            fprintf( stderr, "WebSocket Negotiation Failed: Client closed connection.\n" );
        }
    } );
    //}
    //}
    
    //session->close( BAD_REQUEST );
}

int main( const int, const char** )
{
    auto resource = make_shared< Resource >( );
    resource->set_path( "/socket" ); ///chat
    resource->set_method_handler( "GET", get_method_handler );
    
    auto settings = make_shared< Settings >( );
    settings->set_port( 1984 );
    
    service = make_shared< Service >( );
    service->publish( resource );
    service->schedule( ping_handler, seconds( 5000 ) );
    service->start( settings );
    
    return EXIT_SUCCESS;
}
