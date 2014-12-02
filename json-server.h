#ifndef _HTTP_SERVER_H_
#define _HTTP_SERVER_H_


///debug levels///
#define DEBUG_NONE 0            //no debugging. Error messages will still be displayed.
#define DEBUG_ERROR 0           //only display error messages.
#define DEBUG_WARNING 1         //display warning messages as well
#define DEBUG_INFO 2            //display info messages as well


///error codes///
#define SIZE_CHANGE            -1
#define STRUCT_NOT_FOUND       -2
#define FDARR_MODIFIED         -3
#define QUIT                   -4
#define BAD_SOCKET             -5
#define POSIX_ERROR            -6
#define INTERNAL_RESPONSE      -7
#define HTTP_CGI               -8


///limitations///
#define BUF_LEN 1024
#define NAME_BUF_LEN 64
#define ENV_BUF_LEN 64
#define AVG_LISTING_LEN 100
#define MAX_TOKS 100
#define FDARR_INIT_LEN 100
#define NUM_ENV_VARS 8
#define NUM_DEBUG_LEVELS 3
#define NUM_HTTP_REQUEST_TYPES 4
#define NUM_HTTP_RESPONSE_TYPES 6
#define NUM_REQUEST_STATES 4
#define NUM_REQ_DECL_PARTS 3


///http error messages//
#define BODY_LISTING_BEGIN "<HTML>\n<HEAD>\n<TITLE>Directory Listing"   \
   "</TITLE>\n</HEAD>\n<BODY>\n<H2>Directory Listing</H2><BR>\n<UL>\n"
#define BODY_LISTING_END "</UL>\n</BODY>\n</HTML>\n"
#define BODY_STATUS_BEGIN "<HTML>\n<HEAD>\n<TITLE>Server Status</TITLE>\n" \
   "</HEAD>\n<BODY>\nAuthor: Jacob Hladky<BR>\n"
#define BODY_STATUS_END "<BR>\n<FORM METHOD=\"GET\" ACTION=\"quit\">\n<INPUT " \
   "TYPE=\"submit\" VALUE=\"Quit Server\"/>\n<INPUT TYPE=\"HIDDEN\" NAME=\"" \
   "confirm\" VALUE=\"1\"/>\n</FORM>\n</BODY>\n</HTML>\n"
#define BODY_400 "<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD><BODY>" \
   "<H1>Bad Request</H1>The request could not be understood by this"    \
   "server.</BODY></HTML>"
#define BODY_403 "<HTML><HEAD><TITLE>HTTP ERROR 403</TITLE></HEAD><BODY>" \
   "403 Forbidden.  Your request could not be completed due to "          \
   "encountering HTTP error number 403.</BODY></HTML>"
#define BODY_404 "<HTML><HEAD><TITLE>HTTP ERROR 404</TITLE></HEAD><BODY>" \
   "404 Not Found.  Your request could not be completed due to "          \
   "encountering HTTP error number 404.</BODY></HTML>"
#define BODY_405 "<HTML><HEAD><TITLE>405 Method Not Allowed</TITLE>" \
   "</HEAD><BODY><H1>Method Not Allowed</H1>The method is not "      \
   "allowed for the requested url.</BODY></HTML>"
#define BODY_500 "<HTML><HEAD><TITLE>HTTP ERROR 500</TITLE></HEAD><BODY>" \
   "500 Internal Server Error.  Your request could not be completed due " \
   "to encountering HTTP error number 500.</BODY></HTML>"


///json messages///
#define BODY_JSON_ABOUT "{\n\"author\":\"Jacob Hladky\", "  \
   "\"email\":\"jhladky@calpoly.edu\", \"major\": \"CPE\"}"
#define BODY_JSON_QUIT "{\n\"result\":\"success\"\n}"
//IMPLEMENTED_1 is a limited version is the full feature set
#define BODY_JSON_IMPLEMENTED_1 "[\n{ \"feature\": \"about\", "   \
   "\"URL\": \"/json/about.json\"},{ \"feature\": \"quit\", "     \
   "\"URL\": \"/json/quit\"},{ \"feature\": \"status\", "         \
   "\"URL\": \"/json/status.json\"}]"
#define BODY_JSON_IMPLEMENTED "[\n{ \"feature\": \"about\", "     \
   "\"URL\": \"/json/about.json\"},{ \"feature\": \"quit\", "     \
   "\"URL\": \"/json/quit\"},{ \"feature\": \"status\", "         \
   "\"URL\": \"/json/status.json\"},{ \"feature\": \"fortune\", " \
   "\"URL\": \"/json/fortune.json\"}]"
#define BODY_JSON_STATUS "{\n\"num_clients\": %d, \"num_requests\": %d, " \
   "\"errors\": %d, \"uptime\": %lf, \"cpu_time\": %lf, "                 \
   "\"memory_used\": %ld\n}"


///all other defines///
#define DEBUG_SET 0xAAAAAAAA
#define USEC_PER_SEC 100000.0


///structs, unions, and enums///
enum CGI_CMD {
   CGI_BADPARAMS,
   CGI_STATUS,
   CGI_DO,
   CGI_GOODBYE
};

enum JSON_CMD {
   JSON_QUIT,
   JSON_ABOUT,
   JSON_IMPLEMENTED,
   JSON_STATUS,
   JSON_FORTUNE
};

enum FDARR_CMD {
   FDARR_GET,
   FDARR_ADD,
   FDARR_MODIFY,
   FDARR_REMOVE,
   FDARR_CLEAN_UP
};

enum CXN_STATE {
   ST_REQUEST_INP,
   ST_LOAD_FILE,
   ST_INTERNAL,
   ST_RESPONSE_HEA,
   ST_RESPONSE_FIL,
   ST_CGI_FIL,
   ST_RESPONSE_FIN
};

enum REQUEST_STATE {
   ST_ZERO_READ,
   ST_ERROR,
   ST_INCOMPLETE,
   ST_FINISHED
};

enum REQUEST_TYPE {
   REQUEST_GET,
   REQUEST_PUT,
   REQUEST_POST,
   REQUEST_DELETE
};

enum RESPONSE_TYPE {
   RESPONSE_OK,                  // 200
   RESPONSE_BAD_REQUEST,         // 400
   RESPONSE_METHOD_NOT_ALLOWED,  // 405
   RESPONSE_FORBIDDEN,           // 403
   RESPONSE_NOT_FOUND,           // 404
   RESPONSE_INTERNAL_ERROR       // 500
};

struct request {
   char buffer[BUF_LEN];
   enum REQUEST_STATE state;
   enum REQUEST_TYPE type;      //this and the two below come from the request declaration
   int httpVersion;             //-1 for HTTP/0.9, 0 for HTTP/1.0, 1 for HTTP/1.1
   char filepath[NAME_BUF_LEN]; //we have our own buffer for this so we don't mess up the main buffer 64B
   unsigned int bytesUsed;
   const char* referer;         //these point to locations in buffer
   const char* userAgent;
   bool keepAlive;
   bool acceptDeflate;
} request;

struct response {
   char buffer[BUF_LEN];
   enum RESPONSE_TYPE type;
   unsigned int bytesUsed;
   unsigned int contentLength;
   unsigned int headerLength;
   const char* contentType; //get this from the file ext
   bool usingDeflate;
   bool keepAlive;
   unsigned int bytesToWrite;
} response;

struct env {
   const char* method;
   char* filepath;
   char* query;
   char* envvars[NUM_ENV_VARS + 1]; //add 1 for the NULL terminator
   int childPid;
   int mySocket;  //a copy of the connection socket... rename to cxnSocket
   int serverSocket; //a copy of the server's socket
} env;

struct connection {
   enum CXN_STATE state;     //the current state of the connection
   int socket;               //fd of our socket
   int file;                 //fd of our file
   char* fileBuf;            //location of the file from mmap, maybe use sendfile... ?
   struct request request;
   struct response response;
   struct env env;
   bool quit; //i don't like this... find a better way
   bool cgi; //maybe make bool, cgi, and a few others into a flags int...
} connection;


///function prototypes///
static int cgi_response(struct response* response, enum CGI_CMD cmd);
static int json_response(struct response* response, enum JSON_CMD cmd);
static int error_response(struct response* response, enum RESPONSE_TYPE type);
static int generate_listing(char* filepath, struct response* response);
static int fdarr_cntl(enum FDARR_CMD cmd, ...);
static int do_cgi(struct env* env);
static int process_request(struct connection* cxn);
static int process_cgi(struct connection* cxn);
static int process_json(struct connection* cxn);
static int process_mysock_events(int socket, short revents);
static int process_cxfile_events(struct connection* cxn, short revents);
static int process_cxsock_events(struct connection* cxn, short revents);
static void make_response_header(struct response* response);
static void print_request(struct request* request);
static void close_connection(struct connection* cxn);
static void reset_connection(struct connection* cxn);
static void clean_exit(int unused);
static void wait_cgi(int unused);
static void add_handler(int signal, void (*handlerFunc)(int));
static void log_access(struct connection* cxn);
static void url_decode(char* url);
static void debug(uint32_t debug, const char* msg, ...);
static char* request_declaration_to_string(const struct request* request);
static enum RESPONSE_TYPE cgi_request(struct env* env, enum CGI_CMD* cmd);
static enum RESPONSE_TYPE make_request_header(struct request* request);
static enum RESPONSE_TYPE parse_request_declaration(struct request* request, char** filepath);
static enum REQUEST_STATE fill_request_buffer(int socket, char* buffer, unsigned int* bytesUsed);
static inline const char* bool_to_string(bool b);
static inline void set_debug_level(uintptr_t debugLevel);
static inline void pexit(const char* str);


///request state prototypes///
static int request_state_zero_read(struct connection* cxn);
static int request_state_error(struct connection* cxn);
static int request_state_incomplete(struct connection* cxn);
static int request_state_finished(struct connection* cxn);


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
