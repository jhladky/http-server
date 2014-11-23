#ifndef __HTTP_SERVER_H
#define __HTTP_SERVER_H


#include <poll.h>
#include "smartalloc.h"
#include <unordered_map>
#include <string>


///debug levels///
#define DEBUG_SET 0xAAAAAAAA
#define DEBUG_NONE 0
#define DEBUG_CRITICAL 0
#define DEBUG_ERROR 1
#define DEBUG_WARNING 2
#define DEBUG_INFO 3


///error codes///
#define UNSUPPORTED_OPERATION  -1
#define STRUCT_SIZE_CHANGE     -2
#define STRUCT_NOT_FOUND       -3
#define UNSUPPORTED_HTTP_OP    -4
#define REQUEST_INCOMPLETE     -5 //not really an error... whatevs
#define REQUEST_FINISHED       -6 //also not an error haha
#define BUFFER_OVERFLOW        -7
#define ZERO_READ              -8
#define FDARR_MODIFIED         -9
#define UNRECOVERABLE          -10
#define CGI_QUIT               -11
#define BAD_SOCKET             -12
#define MALLOC_FAILED          -13
#define INTERNAL_RESPONSE      -14
#define BAD_REQUEST            -15


///limitations///
#define BUF_LEN 1024
#define NAME_BUF_LEN 30
#define MAX_TOKS 100
#define AVG_LISTING_LEN 100
#define NUM_ENV_VARS 8
#define ENV_BUF_LEN 50


///all other defines///
#define MAX_HTTP_CODE 600
#define HTTP_OK 200
#define HTTP_NOTFOUND 404
#define HTTP_DENIED 403
#define HTTP_INTERNAL 500
//these aren't HTTP error codes, but we pass them
//into make_response anyway. kludgy, but unambiguous
//as HTTP codes stop at 600
#define HTTP_GENLIST 1001 //V these are internal response codes 
#define HTTP_BADPARAMS 1002
#define HTTP_CGISTATUS 1003
#define HTTP_CGI 1004
#define HTTP_GOODBYE 1005
#define LEN_INDEX_HTML 10
#define LEN_DOCS 4
#define LEN_CGI 4
#define LEN_CGI_BIN 9
#define BODY_LISTING_BEGIN "<HTML>\n<HEAD>\n<TITLE>Directory Listing</TITLE>\n" \
   "</HEAD>\n<BODY>\n<H2>Directory Listing</H2><BR>\n<UL>\n"
#define BODY_LISTING_END "</UL>\n</BODY>\n</HTML>\n"
#define BODY_STATUS_BEGIN "<HTML>\n<HEAD>\n<TITLE>Server Status</TITLE>\n"      \
   "</HEAD>\n<BODY>\nAuthor: Jacob Hladky<BR>\n"
#define BODY_STATUS_END "<BR>\n<FORM METHOD=\"GET\" ACTION=\"quit\">\n<INPUT "  \
   "TYPE=\"submit\" VALUE=\"Quit Server\"/>\n<INPUT TYPE=\"HIDDEN\" NAME=\""    \
   "confirm\" VALUE=\"1\"/>\n</FORM>\n</BODY>\n</HTML>\n"
#define BODY_403 "<HTML><HEAD><TITLE>HTTP ERROR 403</TITLE></HEAD><BODY>" \
   "403 Forbidden.  Your request could not be completed due to "          \
   "encountering HTTP error number 403.</BODY></HTML>"
#define BODY_404 "<HTML><HEAD><TITLE>HTTP ERROR 404</TITLE></HEAD><BODY>" \
   "404 Not Found.  Your request could not be completed due to "          \
   "encountering HTTP error number 404.</BODY></HTML>"
#define BODY_500 "<HTML><HEAD><TITLE>HTTP ERROR 500</TITLE></HEAD><BODY>" \
   "500 Internal Server Error.  Your request could not be completed due " \
   "to encountering HTTP error number 500.</BODY></HTML>"


///structs, unions, and enums///
enum CMD {GET, ADD, MODIFY, REMOVE, CLEAN_UP};

enum STATE {REQUEST_INP, LOAD_FILE, INTERNAL, RESPONSE_HEA, RESPONSE_FIL, CGI_FIL, RESPONSE_FIN};

struct request {
   char buffer[BUF_LEN];
   unsigned int bytesUsed;
   unsigned int contentLength;
   char* line;                  //the HTTP request line
   const char* referer;         //we malloc line but we don't have to 
   const char* userAgent;       //malloc these so they're const
   char* filepath;
   bool keepAlive;
   bool acceptDeflate;
} request;

struct response {
   char buffer[BUF_LEN];
   unsigned int bytesUsed;
   unsigned int contentLength;
   unsigned int headerLength;
   const char* contentType; //get this from the file ext
   bool usingDeflate;
   unsigned short httpCode; //200, 403, 404, etc.
   unsigned int bytesToWrite;
} response;

struct env {
   char* filepath;
   char* method;
   char* query;
   char* envvars[NUM_ENV_VARS + 1]; //add 1 for the NULL terminator
   int childPid;
   int mySocket;  //a copy of the connection socket
   int serverSocket; //a copy of the server's socket
} env;

struct connection {
   enum STATE state;         //the current state of the connection
   int socket;               //fd of our socket
   int file;                 //fd of our file
   char* fileBuf;            //location of the file from mmap, maybe use sendfile... ?
   struct request request;   
   struct response response;
   struct env env;
   bool quit; //i don't like this... find a better way
   bool cgi; //maybe make bool, cgi, and a few others into a flags int...
} connection;


///c++ typedefs///
typedef std::unordered_map<
   std::string,
   std::string,
   std::hash<std::string>,
   std::equal_to<std::string>,
   STLsmartalloc<std::pair<const std::string, std::string>>
> ext_x_mime_t;

typedef std::unordered_map<
   int,
   struct connection *, 
   std::hash<int>, 
   std::equal_to<int>, 
   STLsmartalloc<std::pair<const int, struct connection *>> 
> connections_t; 

#endif
