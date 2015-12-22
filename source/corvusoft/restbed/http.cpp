/*
 * Copyright (c) 2013, 2014, 2015 Corvusoft
 */

//System Includes
#include <map>
#include <regex>
#include <memory>
#include <string>
#include <vector>
#include <ciso646>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <algorithm>

//Project Includes
#include "corvusoft/restbed/http.hpp"
#include "corvusoft/restbed/string.hpp"
#include "corvusoft/restbed/request.hpp"
#include "corvusoft/restbed/response.hpp"
#include "corvusoft/restbed/ssl_settings.hpp"
#include "corvusoft/restbed/detail/request_impl.hpp"
#include "corvusoft/restbed/detail/response_impl.hpp"

//External Includes
#include <asio.hpp>

//System Namespaces
using std::stoi;
using std::stod;
using std::copy;
using std::regex;
using std::string;
using std::smatch;
using std::istream;
using std::ostream;
using std::multimap;
using std::to_string;
using std::shared_ptr;
using std::make_shared;
using std::runtime_error;
using std::ostringstream;
using std::ostream_iterator;
using std::istreambuf_iterator;

//Project Namespaces
using restbed::detail::ResponseImpl;

//External Namespaces
using asio::buffer; //etc...

namespace restbed
{
    shared_ptr< const Response > Http::sync( const shared_ptr< const Request >& request )
    {
        request->m_pimpl->m_io_service = make_shared< asio::io_service >( );
        asio::ip::tcp::resolver resolver( *request->m_pimpl->m_io_service );
        asio::ip::tcp::resolver::query query( request->get_host( ), ::to_string( request->get_port( ) ) );
        asio::ip::tcp::resolver::iterator endpoint_iterator = resolver.resolve( query );
        static const asio::ip::tcp::resolver::iterator end;

        if ( request->m_pimpl->m_socket == nullptr or not request->m_pimpl->m_socket->is_open( ) )
        {
            request->m_pimpl->m_socket = make_shared< asio::ip::tcp::socket >( *request->m_pimpl->m_io_service );
        }

        asio::error_code error = asio::error::host_not_found;
        
        do
        {
            request->m_pimpl->m_socket->close( ); //would this not kill keep alive sockets requests?
            request->m_pimpl->m_socket->connect( *endpoint_iterator++, error );
        }
        while ( error and endpoint_iterator not_eq end );
        
        if ( error )
        {
        	throw runtime_error( String::format( "Failed to locate interface: %s\n", error.message( ).data( ) ) );
        }
        
        const auto data = to_bytes( *request );

        request->m_pimpl->m_buffer = make_shared< asio::streambuf >( );
        ostream request_stream( request->m_pimpl->m_buffer.get( ) );
        copy( data.begin( ), data.end( ), ostream_iterator< Byte >( request_stream ) );
        asio::write( *request->m_pimpl->m_socket, *request->m_pimpl->m_buffer, error );

        if ( error )
        {
        	throw runtime_error( String::format( "Failed to transmit request: %s\n", error.message( ).data( ) ) );
        }
        
        istream response_stream( request->m_pimpl->m_buffer.get( ) );
        asio::read_until( *request->m_pimpl->m_socket, *request->m_pimpl->m_buffer, "\r\n", error );

        if ( error )
        {
            throw runtime_error( String::format( "Failed to receive response: %s\n", error.message( ).data( ) ) );
        }

        string status_line = String::empty;
        getline( response_stream, status_line );

        smatch matches;
        static const regex status_line_pattern( "^([a-zA-Z]+)\\/(\\d*\\.?\\d*) (-?\\d+) (.+)\\r$" );

        if ( not regex_match( status_line, matches, status_line_pattern ) or matches.size( ) not_eq 5 )
        {
        	throw runtime_error( String::format( "Failed with malformed status line: '%s'\n", status_line.data( ) ) );
        }

        auto response = make_shared< Response >( );
        response->m_pimpl->m_request = request;
        request->m_pimpl->m_response = response;
        response->set_protocol( matches[ 1 ].str( ) );
        response->set_version( stod( matches[ 2 ].str( ) ) );
        response->set_status_code( stoi( matches[ 3 ].str( ) ) );
        response->set_status_message( matches[ 4 ].str( ) );

        asio::read_until( *request->m_pimpl->m_socket, *request->m_pimpl->m_buffer, "\r\n\r\n", error );

        if ( error == asio::error::eof )
        {
            return response;
        }

        if ( error )
        {
        	throw runtime_error( String::format( "Failed to receive response headers: '%s'\n", error.message( ).data( ) ) );
        }

        string header = String::empty;
        multimap< string, string > headers = { };
        
        while ( getline( response_stream, header ) and header not_eq "\r" )
        {
            static const regex header_pattern( "^([^:.]*): *(.*)\\s*$" );
            
            if ( not regex_match( header, matches, header_pattern ) or matches.size( ) not_eq 3 )
            {
            	throw runtime_error( String::format( "Failed with malformed header: '%s'\n", header.data( ) ) );
            }

            headers.insert( make_pair( matches[ 1 ], matches[ 2 ] ) );
        }

        response->set_headers( headers );

        return response;
    }

    Bytes Http::fetch( const size_t length, const shared_ptr< const Response >& response )
    {
        Bytes data = { };
        auto request = response->get_request( );

        if ( length > request->m_pimpl->m_buffer->size( ) )
        {
            asio::error_code error;
            const size_t adjusted_length = length - request->m_pimpl->m_buffer->size( );

            const size_t size = asio::read( *request->m_pimpl->m_socket, *request->m_pimpl->m_buffer, asio::transfer_at_least( adjusted_length ), error );

            if ( error and error not_eq asio::error::eof )
            {
                throw runtime_error( String::format( "Failed to receive response body: '%s'\n", error.message( ).data( ) ) );
            }

            const auto data_ptr = asio::buffer_cast< const Byte* >( request->m_pimpl->m_buffer->data( ) );
            data = Bytes( data_ptr, data_ptr + size );
            request->m_pimpl->m_buffer->consume( size );
        }
        else
        {
            const auto data_ptr = asio::buffer_cast< const Byte* >( request->m_pimpl->m_buffer->data( ) );
            data = Bytes( data_ptr, data_ptr + length );
            request->m_pimpl->m_buffer->consume( length );
        }

        auto& body = response->m_pimpl->m_body;

        if ( body.empty( ) )
        {
            body = data;
        }
        else
        {
            body.insert( body.end( ), data.begin( ), data.end( ) );
        }

        return data;
    }

    Bytes Http::fetch( const string& delimiter, const shared_ptr< const Response >& response )
    {
        asio::error_code error;
        auto request = response->m_pimpl->m_request;
        const size_t size = asio::read_until( *response->get_request( )->m_pimpl->m_socket, *request->m_pimpl->m_buffer, delimiter, error );

        if ( error )
        {
            throw runtime_error( String::format( "Failed to receive response body: '%s'\n", error.message( ).data( ) ) );
        }

        const auto data_ptr = asio::buffer_cast< const Byte* >( request->m_pimpl->m_buffer->data( ) );
        const Bytes data( data_ptr, data_ptr + size );
        request->m_pimpl->m_buffer->consume( size );

        auto& body = response->m_pimpl->m_body;

        if ( body.empty( ) )
        {
            body = data;
        }
        else
        {
            body.insert( body.end( ), data.begin( ), data.end( ) );
        }

        return data;
    }

    Bytes Http::to_bytes( const Request& request )
    {
        string query = String::empty;

        for ( const auto parameter : request.get_query_parameters( ) )
        {
           query += parameter.first + "=" + parameter.second + "&";
        }

        if ( query.back( ) == '&' )
        {
            query = "?" + query.substr( 0, query.length( ) - 1 );
        }

        ostringstream stream;
        stream << request.get_method( )      << " ";
        stream << request.get_path( )              ;
        stream << query                      << " ";
        stream << request.get_protocol( )    << "/";
        stream << request.get_version( )  << "\r\n";

        for ( const auto header : request.get_headers( ) )
        {
            stream << header.first << ": " << header.second << "\r\n";
        }

        const auto data = stream.str( ) + "\r\n";
        Bytes result( data.begin( ), data.end( ) );

        Bytes body = request.get_body( );
        result.insert( result.end( ), body.begin( ), body.end( ) );

        return result;
    }
}
