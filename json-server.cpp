#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <netdb.h>
#include <dirent.h>
#include <poll.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <netdb.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/ip.h>

#include <unordered_map>
#include <string>

#include "smartalloc.h"
#include "json-server.h"

static connections_t* connections = new connections_t(500);
static FILE* accessLog; //don't like this being global
static ext_x_mime_t* ext2mime = new ext_x_mime_t({
      {"html", "text/html"},
      {"htm", "text/html"},
      {"jpeg", "image/jpeg"},
      {"jpg", "image/jpeg"},
      {"png", "image/png"},
      {"gif", "image/gif"},
      {"pdf", "application/pdf"},
      {"json", "application/json"},
      {"txt", "text/plain"}});

//lovin dat global data
static int numClients = 0;
static int numRequests = 0;
static int errors = 0;
static struct timeval startTime;


int main(int argc, char* argv[]) {
   struct sockaddr_in6 me;
   int mySock, res;
   char v6buf[INET6_ADDRSTRLEN];
   socklen_t sockSize = sizeof(struct sockaddr_in6);

   set_debug_level(DEBUG_INFO);
   //set_debug_level(DEBUG_NONE);
   debug(DEBUG_INFO, "Server Starting...\n");
   
   add_handler(SIGINT, clean_exit);
   add_handler(SIGCHLD, wait_cgi);

   if (gettimeofday(&startTime, NULL) == -1) {
      pexit("gettimeofday");
   }

   if ((accessLog = fopen("access.log", "a")) == NULL) {
      pexit("fopen");
   }

   if ((mySock = socket(AF_INET6, SOCK_STREAM, 0)) == -1) {
      pexit("Opening socket failed");
   }

   // Make the socket non-blocking.
   if (fcntl(mySock, F_SETFL, fcntl(mySock, F_GETFL) | O_NONBLOCK) == -1) {
      pexit("fcntl");
   }

   memset(&me, 0, sizeof(me));
   me.sin6_family = AF_INET6;
   if (argc > 1) {
      if ((res = inet_pton(AF_INET6, argv[1], &me.sin6_addr)) == -1) {
         pexit("inet_pton");
      } else if (res == 0) {
         debug(DEBUG_WARNING, "Converting v4 to v6 addr\n");
         snprintf(v6buf, INET6_ADDRSTRLEN, "::ffff:%s", argv[1]);
         if (inet_pton(AF_INET6, v6buf, &me.sin6_addr) == 0) {
            debug(DEBUG_ERROR, "Invalid address.\n");
            exit(EXIT_FAILURE);
         }
      }
   } else {
      me.sin6_addr = in6addr_any;
   }

   if (bind(mySock, (struct sockaddr *) &me, sockSize)) {
      pexit("bind");
   }

   if (getsockname(mySock, (struct sockaddr *) &me, &sockSize)) {
      pexit("getsockname");
   }

   printf("HTTP server is using TCP port %d\n", ntohs(me.sin6_port));
   fflush(stdout);

   if (listen(mySock, SOMAXCONN)) {
      pexit("listen");
   }

   if (fdarr_cntl(FDARR_ADD, mySock, POLLIN) == POSIX_ERROR) {
      debug(DEBUG_ERROR, "File descriptor array modification failed.\n");
      exit(EXIT_FAILURE);
   }

   //the event loop
   while (true) {
      struct pollfd* fds;
      nfds_t nfds;
      unsigned int i;
      int res, fd;

      fdarr_cntl(FDARR_GET, &fds, &nfds);
      if ((res = poll(fds, nfds, -1)) == -1) {
         debug(DEBUG_WARNING, "Poll: %s\n", strerror(errno));
         continue; //not a fan of this
      }

      for (i = 0; i < nfds; i++) {
         fd = fds[i].fd;

         if (fd != mySock && connections->count(fd) == 0) {
            debug(DEBUG_ERROR, "Unrecognized fd(%d) in fdarr\n", fd);
            exit(EXIT_FAILURE);
         }

         if (fd == mySock) {
            res = process_mysock_events(mySock, fds[i].revents);
         } else if (fd == connections->at(fd)->socket) {
            res = process_cxsock_events(connections->at(fd), fds[i].revents);
         } else if (fd == connections->at(fd)->file) {
            res = process_cxfile_events(connections->at(fd), fds[i].revents);
         }

         //if the fdarr was modified, break from the loop
         //and start over completely with the right nfds value
         if (res == FDARR_MODIFIED) {
            debug(DEBUG_INFO, "Fdarr modified, breaking\n");
            break;
         } else if (res == BAD_SOCKET) {
            debug(DEBUG_WARNING, "Error accepting socket connection\n");
         } else if (res == POSIX_ERROR) {
            debug(DEBUG_ERROR, "Allocating connections struct failed\n");
            exit(EXIT_FAILURE);
         }
      }
   }

   return 0;
}

/* (1) Initialize connection struct to 0
 * (2) We can't generate an internal error b.c
 *     we can't communicate with the client...
 *     do just return and hope it doesn't happen again
 */
static int process_mysock_events(int socket, short revents) {
   struct connection* cxn;
   struct sockaddr_in6 me;
   socklen_t sockSize = sizeof(struct sockaddr_in6);
   char v6buf[INET6_ADDRSTRLEN];
   int err = 0;

   if (revents & POLLIN) {
      debug(DEBUG_INFO, "mysock, revent: POLLIN, state: N/A\n");
      cxn = (struct connection *) calloc(1, sizeof(struct connection)); //(1)
      if (cxn == NULL) {
         return POSIX_ERROR;
      }

      cxn->socket = accept(socket, (struct sockaddr *) &me, &sockSize);
      if (cxn->socket == -1) {
         debug(DEBUG_WARNING, "accept: %s\n", strerror(errno));
         return BAD_SOCKET; //(2)
      }

      cxn->state = ST_REQUEST_INP;
      cxn->response.type = RESPONSE_OK;
      cxn->env.mySocket = cxn->socket;
      cxn->env.serverSocket = socket;
      debug(DEBUG_INFO, "Connected to %s\n",
            inet_ntop(AF_INET6, &me.sin6_addr, v6buf, INET6_ADDRSTRLEN));
      numClients++;

      fdarr_cntl(FDARR_ADD, cxn->socket, POLLIN | POLLOUT);
      err = FDARR_MODIFIED;
      connections->insert(std::make_pair(cxn->socket, cxn));
      debug(DEBUG_INFO, "Allocated connection struct(%d)\n", cxn->socket);
   }

   return err;
}


static int process_cxsock_events(struct connection* cxn, short revents) {
   static int (*frbActions[NUM_REQUEST_STATES])(struct connection *) = {
      request_state_zero_read,
      request_state_error,
      request_state_incomplete,
      request_state_finished
   };
   int res, err = 0;
   char* ptr;

   if (revents & POLLIN && cxn->state == ST_REQUEST_INP) {
      debug(DEBUG_INFO, "cxsock, revent: POLLIN, state: ST_REQUEST_INP\n");
      return frbActions[fill_request_buffer(cxn->socket,
                                            cxn->request.buffer,
                                            &cxn->request.bytesUsed)](cxn);
   } else if (revents & POLLOUT && cxn->state == ST_INTERNAL) {
      debug(DEBUG_INFO, "cxsock, revent: POLLOUT, state: ST_RESPONSE_HEA\n");

      res = write(cxn->socket,
                  cxn->response.buffer,
                  cxn->response.bytesToWrite);
      cxn->state = cxn->cgi ? ST_CGI_FIL : ST_RESPONSE_FIN;
   } else if (revents & POLLOUT && cxn->state == ST_RESPONSE_HEA) {
      debug(DEBUG_INFO, "cxsock, revent: POLLOUT, state: ST_RESPONSE_HEA\n");

      res = write(cxn->socket,
                  cxn->response.buffer,
                  cxn->response.headerLength);
      cxn->response.bytesToWrite -= res;
      cxn->state = res == -1 ? ST_RESPONSE_FIN : ST_RESPONSE_FIL;
   } else if (revents & POLLOUT && cxn->state == ST_RESPONSE_FIL) {
      debug(DEBUG_INFO, "cxsock, revent: POLLOUT, state: RESPONSE_INP\n");
      ptr = cxn->fileBuf + cxn->response.contentLength -
         cxn->response.bytesToWrite;
      res = write(cxn->socket, ptr, cxn->response.bytesToWrite);

      if (res == -1) {
         cxn->state = ST_RESPONSE_FIN;
      } else {
         cxn->response.bytesToWrite -= res;
         cxn->state = cxn->response.bytesToWrite ? ST_LOAD_FILE : ST_RESPONSE_FIN;
      }
   } else if (revents & POLLOUT && cxn->state == ST_CGI_FIL) {
      if (do_cgi(&cxn->env) == POSIX_ERROR) {
         //internal server error here
         //we'd need to go backwards in the state machine
      }
      cxn->response.type = RESPONSE_OK;
      cxn->state = ST_RESPONSE_FIN;
      return process_cxsock_events(cxn, POLLOUT); //fix me later
   } else if (revents & POLLOUT && cxn->state == ST_RESPONSE_FIN) {
      debug(DEBUG_INFO, "cxsock, revent: POLLOUT, state: ST_RESPONSE_FIN\n");

      log_access(cxn);

      if (cxn->quit) {
         clean_exit(0);
      } else if (cxn->request.keepAlive && cxn->response.type == RESPONSE_OK) {
         reset_connection(cxn);
         cxn->state = ST_REQUEST_INP;
      } else {
         debug(DEBUG_INFO, "Keep alive not specified or "
               "http error encountered; Closing connection\n");
         close_connection(cxn);
      }
      err = FDARR_MODIFIED;
   } else if (revents & POLLHUP || revents & POLLERR || revents & POLLNVAL) {
      debug(DEBUG_INFO, "cxsock, revent: POLLHUP/POLLERR/POLLNVAL\n");
      close_connection(cxn);
      err = FDARR_MODIFIED;
   }

   return err;
}

//(1) We don't need to do anything, this is just a
//    condition to be able to write back onto the socket
static int process_cxfile_events(struct connection* cxn, short revents) {
   int err = 0;

   if (revents & POLLIN && cxn->state == ST_LOAD_FILE) {
      debug(DEBUG_INFO, "cxfile, revent: POLLIN, state: ST_LOAD_FILE\n");
      cxn->state = ST_RESPONSE_FIL; //(1)
   }

   return err;
}

static int request_state_zero_read(struct connection* cxn) {
   debug(DEBUG_INFO, "Zero read, closing connection\n");
   close_connection(cxn);
   return FDARR_MODIFIED;
}

static int request_state_error(struct connection* cxn) {
   debug(DEBUG_WARNING, "Error processing request...sending 500!\n");
   cxn->state = ST_INTERNAL;
   error_response(&cxn->response, RESPONSE_INTERNAL_ERROR);
   return 0;
}

static int request_state_incomplete(struct connection* cxn) {
   // do nothing!
   return 0;
}

static int request_state_finished(struct connection* cxn) {
   char* ptr;

   numRequests++;
   switch(process_request(cxn)) {
   case HTTP_CGI:
      cxn->cgi = true;
      cxn->state = ST_INTERNAL;
      return 0;
   case QUIT:
      cxn->quit = true;
      cxn->state = ST_INTERNAL;
      return 0;
   case INTERNAL_RESPONSE:
      cxn->state = ST_INTERNAL;
      return 0;
   }

   cxn->fileBuf = (char *) mmap(NULL, cxn->response.contentLength,
                                PROT_READ, MAP_PRIVATE, cxn->file, 0);
   if ((intptr_t) cxn->fileBuf == -1) {
      debug(DEBUG_WARNING, "mmap: %s\n", strerror(errno));
      cxn->state = ST_INTERNAL;
      close(cxn->file);
      error_response(&cxn->response, RESPONSE_INTERNAL_ERROR);
      return 0;
   } else {
      cxn->state = ST_RESPONSE_HEA;
      fdarr_cntl(FDARR_ADD, cxn->file, POLLIN);
      connections->insert(std::make_pair(cxn->file, cxn));
      debug(DEBUG_INFO, "Adding fd to struct, #%d\n", cxn->file);
      ptr = strrchr(cxn->request.filepath, '.');
      if (ptr && ext2mime->count(std::string(ptr + 1))) {
         cxn->response.contentType =
            ext2mime->at(std::string(ptr +1)).c_str();
      }

      make_response_header(&cxn->response);
      return FDARR_MODIFIED;
   }
}

static int process_cgi(struct connection* cxn) {
   struct response* response = &cxn->response;
   enum CGI_CMD cmd;

   //we're using the actual errno define here
   switch(cgi_request(&cxn->env, &cmd)) {
   case FILE_NOT_FOUND: return error_response(response, RESPONSE_NOT_FOUND);
   case EACCES: return error_response(response, RESPONSE_FORBIDDEN);
   case POSIX_ERROR: return error_response(response, RESPONSE_INTERNAL_ERROR);
   }

   debug(DEBUG_INFO, "Doing CGI action.\n");
   return cgi_response(&cxn->response, cmd);
}

static int process_json(struct connection* cxn) {
   enum JSON_CMD cmd;
   char* str = cxn->env.query + LEN_JSON;

   if (strcmp(cxn->env.query, "/json/") == 0) {
      return error_response(&cxn->response, RESPONSE_NOT_FOUND);
   } else if (strcmp(str, "quit") == 0) {
      cmd = JSON_QUIT;
   } else if (strcmp(str, "about.json") == 0) {
      cmd = JSON_ABOUT;
   } else if (strcmp(str, "implemented.json") == 0) {
      cmd = JSON_IMPLEMENTED;
   } else if (strcmp(str, "status.json") == 0) {
      cmd = JSON_STATUS;
   } else if (strcmp(str, "fortune.json") == 0) {
      cmd = JSON_FORTUNE;
   } else {
      debug(DEBUG_WARNING, "Unsupported JSON operation requested: %s\n", str);
      return error_response(&cxn->response, RESPONSE_NOT_FOUND);
   }

   debug(DEBUG_INFO, "Doing some JSON action.\n");
   return json_response(&cxn->response, cmd);
}

static int process_request(struct connection* cxn) {
   char* tmp = cxn->request.filepath + LEN_DOCS;
   struct stat stats;
   struct env* env = &cxn->env;
   struct request* request = &cxn->request;
   struct response* response = &cxn->response;
   int res;

   debug(DEBUG_INFO, "Processing request buffer\n");
   res = make_request_header(request);
   print_request(request);

   //we're not supporing deflate right now
   response->usingDeflate = false;
   response->keepAlive = request->keepAlive;

   /* This is really three separate errors that should result in three separate
    * different types of HTTP responses, 405, 500, and 400, respectively
    * but we're combining them into a 500 right now.
    */
   switch (res) {
   case NOT_ALLOWED: return error_response(response, RESPONSE_METHOD_NOT_ALLOWED);
   case BUFFER_OVERFLOW: return error_response(response, RESPONSE_INTERNAL_ERROR);
   case BAD_REQUEST:return error_response(response, RESPONSE_BAD_REQUEST);
   }

   if ((strstr(tmp, "/cgi-bin/"))) { //check for cgi
      debug(DEBUG_INFO, "cgi request: %s\n", tmp);
      env->method = "GET"; // fix....
      env->query = tmp;
      return process_cgi(cxn);
   }

   if (strstr(tmp, "/json/")) { //check for json
      debug(DEBUG_INFO, "json request: %s\n", tmp);
      env->query = tmp;
      return process_json(cxn);
   }

   if (stat(request->filepath, &stats) == -1) {
      switch (errno) {
      case ENOENT: return error_response(response, RESPONSE_NOT_FOUND);
      case EACCES: return error_response(response, RESPONSE_FORBIDDEN);
      default:
         debug(DEBUG_WARNING, "stat: %s\n", strerror(errno));
         return error_response(response, RESPONSE_INTERNAL_ERROR);
      }
   }

   if (S_ISDIR(stats.st_mode)) {
      debug(DEBUG_INFO, "Is a directory.\n");
      if (request->filepath[strlen(request->filepath) - 1] != '/') {
         strcat(request->filepath, "/");
      }
      strcat(request->filepath, "index.html");

      if (stat(request->filepath, &stats) == -1) {
         switch (errno) {
         case ENOENT:
            debug(DEBUG_INFO, "Generating directory listing.\n");
            return generate_listing(request->filepath, response);
         default:
            debug(DEBUG_WARNING, "stat: %s\n", strerror(errno));
            return error_response(response, RESPONSE_INTERNAL_ERROR);
         }
      }
   }

   response->contentLength = stats.st_size;
   if ((res = open(request->filepath, O_RDONLY | O_NONBLOCK)) == -1) {
      switch (errno) {
      case EACCES: return error_response(response, RESPONSE_FORBIDDEN);
      default:
         debug(DEBUG_WARNING, "popen: %s\n", strerror(errno));
         return error_response(response, RESPONSE_INTERNAL_ERROR);
      }
   }

   //debug(DEBUG_WARNING, "Content-Length is %d\n", response->contentLength);
   cxn->file = res;
   return 0;
}

static void reset_connection(struct connection* cxn) {
   int socket = cxn->socket;
   struct env env;

   debug(DEBUG_INFO, "Resetting connection\n");
   env = cxn->env;
   /* 
    * If there was an entry for the file in the file descriptor table, then
    * it is also open and in other data structures, so remove it from them.
    * If it was not in the table then erase will simply return 0. A file
    * descriptor may not be set in the connection because we might have done
    * an internal response such as a 404 in response to the request, instead of
    * sending a file.
    */
   if (connections->erase(cxn->file)) {
      fdarr_cntl(FDARR_REMOVE, cxn->file);
      close(cxn->file);
   }
   munmap(cxn->fileBuf, cxn->response.contentLength);
   memset(cxn, 0, sizeof(struct connection));
   cxn->socket = socket;
   cxn->env = env;
}

static void close_connection(struct connection* cxn) {
   debug(DEBUG_INFO, "Closing connection...\n");

   fdarr_cntl(FDARR_REMOVE, cxn->socket);
   if (connections->erase(cxn->file)) {
      fdarr_cntl(FDARR_REMOVE, cxn->file);
      close(cxn->file);
   }
   close(cxn->socket);
   connections->erase(cxn->socket);
   debug(DEBUG_INFO, "Freeing connection struct\n");
   free(cxn);
}

static enum REQUEST_STATE fill_request_buffer(int socket, char* buffer, unsigned int* bytesUsed) {
   int bytesRead = 0;

   if ((bytesRead = read(socket, buffer + *bytesUsed, BUF_LEN - *bytesUsed)) == -1) {
      debug(DEBUG_WARNING, "read: %s\n", strerror(errno));
      return ST_ERROR;
   } else if (bytesRead == 0) {
      debug(DEBUG_WARNING, "Zero read from socket\n");
      return ST_ZERO_READ;
   }

   *bytesUsed += bytesRead;

   if (*bytesUsed >= BUF_LEN) {
      debug(DEBUG_WARNING, "Request buffer full.\n");
      return ST_ERROR;
   }

   if (memmem(buffer, *bytesUsed, "\r\n\r\n", strlen("\r\n\r\n")) == NULL) {
      debug(DEBUG_INFO, "Request incomplete.\n");
      return ST_INCOMPLETE;
   } else {
      debug(DEBUG_INFO, "End of request found\n");
      return ST_FINISHED;
   }
}

static int fdarr_cntl(enum FDARR_CMD cmd, ...) {
   static struct pollfd* fds = NULL;
   static nfds_t nfds = 0;
   static unsigned int fdarrMaxSize = FDARR_INIT_LEN / 2;
   struct pollfd** fdsFill;
   nfds_t* nfdsFill;
   unsigned int i;
   int fd, err;
   va_list args;

   va_start(args, cmd);
   if (nfds >= fdarrMaxSize - 1 || fds == NULL) {
      fdarrMaxSize *= 2;
      debug(DEBUG_INFO, "Expanding pollfd array from "
            "size %d to size %d\n", nfds, fdarrMaxSize);
      fds = (struct pollfd *) realloc(fds,
                                      fdarrMaxSize * sizeof(struct pollfd));
      if (fds == NULL) {
         debug(DEBUG_WARNING, "Allocating pollfd array failed\n");
         return POSIX_ERROR;
      }
      err = SIZE_CHANGE;
   }

   switch(cmd) {
   default:
   case FDARR_GET:
      fdsFill = va_arg(args, struct pollfd **);
      nfdsFill = va_arg(args, nfds_t *);

      *fdsFill = fds;
      *nfdsFill = nfds;
      break;
   case FDARR_ADD:
      fds[nfds].fd = va_arg(args, int);
      debug(DEBUG_INFO, "Adding %dth entry(%d) to "
            "pollfd array\n", nfds, fds[nfds].fd);
      fds[nfds].events = (short) va_arg(args, int);
      nfds++;
      break;
   case FDARR_MODIFY:
      fd = va_arg(args, int);
      for (i = 0; i < nfds; i++) {
         if (fds[i].fd == fd) {
            break;
         }
      }

      fds[i].events = (short) va_arg(args, int);
      break;
   case FDARR_REMOVE:
      fd = va_arg(args, int);
      for (i = 0; i < nfds; i++) {
         if (fds[i].fd == fd) {
            break;
         }
      }

      if (i == nfds) {
         debug(DEBUG_WARNING, "Entry not found in pollfd array\n");
         return STRUCT_NOT_FOUND;
      }

      debug(DEBUG_INFO, "Removing %dth entry(%d) from "
            "pollfd array\n", i, fd);
      if (i != nfds - 1 && nfds > 1) {
         memcpy(fds + i, fds + nfds - 1, sizeof(struct pollfd));
      }
      nfds--;
      break;
   case FDARR_CLEAN_UP:
      debug(DEBUG_INFO, "Freeing pollfd array\n");
      free(fds);
      break;
   }

   va_end(args);
   return err;
}

static int json_response(struct response* response, enum JSON_CMD cmd) {
   int bytesWritten = 0;
   long memUsage;
   double cpuTime, timeDiff;
   struct rusage usage;
   struct timeval now;
   char buf[BUF_LEN];

   switch(cmd) {
   case JSON_IMPLEMENTED:
      debug(DEBUG_INFO, "JSON: doing implemented.json\n");
      bytesWritten = sprintf(buf, "%s", BODY_JSON_IMPLEMENTED_1);
      break;
   case JSON_ABOUT:
      debug(DEBUG_INFO, "JSON: doing about.json\n");
      bytesWritten = sprintf(buf, "%s", BODY_JSON_ABOUT);
      break;
   case JSON_QUIT:
      debug(DEBUG_INFO, "JSON: doing quit\n");
      bytesWritten = sprintf(buf, "%s", BODY_JSON_QUIT);
      break;
   case JSON_STATUS:
      debug(DEBUG_INFO, "JSON: doing status.json\n");
      if (getrusage(RUSAGE_SELF, &usage) == -1 ||
          gettimeofday(&now, NULL) == -1) {
         return error_response(response, RESPONSE_INTERNAL_ERROR);
      }
      memUsage = usage.ru_ixrss + usage.ru_idrss + usage.ru_isrss;
      cpuTime = (usage.ru_utime.tv_sec * USEC_PER_SEC +
                 usage.ru_stime.tv_sec * USEC_PER_SEC +
                 usage.ru_utime.tv_usec +
                 usage.ru_stime.tv_usec) / USEC_PER_SEC;
      timeDiff = ((now.tv_sec * USEC_PER_SEC + now.tv_usec) -
                  (startTime.tv_sec * USEC_PER_SEC +
                   startTime.tv_usec)) / USEC_PER_SEC;
      bytesWritten = sprintf(buf, BODY_JSON_STATUS, numClients, numRequests,
                             errors, timeDiff, cpuTime, memUsage);
      break;
   case JSON_FORTUNE:
      debug(DEBUG_INFO, "JSON: doing fortune.json\n");
      break;
   }

   response->type = RESPONSE_OK;
   response->contentLength = bytesWritten;
   response->contentType = "application/json";

   make_response_header(response);
   memcpy(response->buffer + response->headerLength, buf, bytesWritten);
   response->bytesUsed += bytesWritten;
   return cmd == JSON_QUIT ? QUIT : INTERNAL_RESPONSE;
}

//an error response is a response that's not the normal HTTP 200 OK
static int error_response(struct response* response, enum RESPONSE_TYPE type) {
   char buf[BUF_LEN];
   int bytesWritten = 0;

   switch(type) {
   case RESPONSE_BAD_REQUEST:
      debug(DEBUG_INFO, "Sending 400 Bad Request\n");
      bytesWritten = sprintf(buf, "%s", BODY_400);
      break;
   case RESPONSE_FORBIDDEN:
      debug(DEBUG_INFO, "Sending 403 Forbidden\n");
      bytesWritten = sprintf(buf, "%s", BODY_403);
      break;
   case RESPONSE_NOT_FOUND:
      debug(DEBUG_INFO, "Sending 404 Not Found\n");
      bytesWritten = sprintf(buf, "%s", BODY_404);
      break;
   case RESPONSE_METHOD_NOT_ALLOWED:
      debug(DEBUG_INFO, "Sending 405 Method Not Allowed\n");
      bytesWritten = sprintf(buf, "%s", BODY_405);
      break;
   default:
   case RESPONSE_INTERNAL_ERROR:
      debug(DEBUG_WARNING, "Sending 500 Internal\n");
      bytesWritten = sprintf(buf, "%s", BODY_500);
      break;
   }

   errors++;
   response->contentLength = bytesWritten;
   response->contentType = "text/html";
   response->type = type;
   make_response_header(response);
   memcpy(response->buffer + response->headerLength, buf, bytesWritten);
   response->bytesUsed += bytesWritten;

   return INTERNAL_RESPONSE;
}

static int cgi_response(struct response* response, enum CGI_CMD cmd) {
   char buf[BUF_LEN];
   time_t t = time(NULL);
   int bytesWritten = 0;

   switch(cmd) {
   case CGI_BADPARAMS:
      errors++;
      debug(DEBUG_INFO, "Sending Bad Params message\n");
      bytesWritten = sprintf(buf, "Bad Params!");
      break;
   case CGI_GOODBYE:
      debug(DEBUG_INFO, "Sending Goodbye message\n");
      bytesWritten = sprintf(buf, "Goodbye!");
      break;
   case CGI_STATUS:
      debug(DEBUG_INFO, "Sending CGI status message\n");
      bytesWritten = sprintf(
         buf, "%sServer Process ID: %d<BR>\nCurrent Time: %s%s",
         BODY_STATUS_BEGIN, getpid(), asctime(localtime(&t)), BODY_STATUS_END);
      break;
   case CGI_DO:
      debug(DEBUG_INFO, "Sending CGI response line\n");
      bytesWritten = sprintf(response->buffer, "HTTP/1.1 200 OK\r\n");
      response->bytesToWrite = response->bytesUsed =
         response->headerLength = bytesWritten;
      return HTTP_CGI;
   }

   response->contentLength = bytesWritten;
   response->contentType = (cmd == CGI_BADPARAMS || cmd == CGI_GOODBYE) ?
      "text/plain" : "text/html";
   response->type = RESPONSE_OK;
   make_response_header(response);
   memcpy(response->buffer + response->headerLength, buf, bytesWritten);
   response->bytesUsed += bytesWritten;
   return cmd == CGI_GOODBYE ? QUIT : INTERNAL_RESPONSE;
}

//overloaded initally
static int make_response_header(struct response* response) {
   static const char* responseType2String[NUM_HTTP_RESPONSE_TYPES] = {
      "200 OK",
      "404 Not Found",
      "404 Not Found",
      "403 Forbidden",
      "404 Not Found",
      "500 Internal Server Error"
   };
   int written;

   written = snprintf(response->buffer, BUF_LEN, "HTTP/1.1 %s\r\n",
                      responseType2String[response->type]);

   if (response->contentType) {
      written += snprintf(response->buffer + written, BUF_LEN - written,
                          "Content-Type: %s\r\n", response->contentType);
   }

   if (response->keepAlive) {
      written += snprintf(response->buffer + written, BUF_LEN - written,
                          "Connection: Keep-Alive\r\n");
   }

   if (response->usingDeflate) {
      written += snprintf(response->buffer + written, BUF_LEN - written,
                          "Content-Encoding: deflate\r\n");
   } else {
      written += snprintf(response->buffer + written, BUF_LEN - written,
                          "Content-Length: %d\r\n\r\n", response->contentLength);
   }
   response->bytesUsed = response->headerLength = written;
   response->bytesToWrite = response->headerLength + response->contentLength;
   return 0;
}

static int generate_listing(char* filepath, struct response* response) {
   char* buf;
   int bytesWritten = 0, numDirEntries = 0;
   DIR* parent;
   char* str = (char *) calloc(1, strlen(filepath) - LEN_DOCS + 1);
   struct dirent* entry;

   debug(DEBUG_INFO, "In generate_listing\n");

   if (str == NULL) {
      debug(DEBUG_WARNING, "Allocating filepath copy failed\n");
      return error_response(response, RESPONSE_INTERNAL_ERROR);
   }

   memcpy(str, filepath + LEN_DOCS, strlen(filepath) - LEN_DOCS + 1);
   *(strstr(str, "/index.html")) = '\0';
   *(strstr(filepath, "/index.html")) = '\0';
   parent = opendir(filepath);
   while ((entry = readdir(parent))) {
      numDirEntries++;
   }
   closedir(parent);

   buf = (char *) calloc(1, AVG_LISTING_LEN * numDirEntries);
   if (buf == NULL) {
      debug(DEBUG_WARNING, "Allocating generate listing buffer failed.\n");
      free(str);
      return error_response(response, RESPONSE_INTERNAL_ERROR);
   }
   //bufLen = AVG_LISTING_LEN * numDirEntries; //why is this commented out??

   bytesWritten = sprintf(buf, "%s", BODY_LISTING_BEGIN);
   parent = opendir(filepath);

   while ((entry = readdir(parent))) {
      if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
         bytesWritten += sprintf(buf + bytesWritten,
                                 "<LI><A HREF=\"%s/%s\">%s</A></LI>\n",
                                 str, entry->d_name, entry->d_name);
      }
   }

   closedir(parent);
   bytesWritten += sprintf(buf + bytesWritten, "%s", BODY_LISTING_END);

   response->contentType = "text/html";
   response->type = RESPONSE_OK;
   response->contentLength = bytesWritten;

   make_response_header(response);
   memcpy(response->buffer + response->headerLength, buf, bytesWritten);
   response->bytesUsed += bytesWritten;

   free(str);
   free(buf);
   return INTERNAL_RESPONSE;
}

//parse the request declaration
//will modify the string in the request buffer via strtok!
static int parse_request_declaration(struct request* request, char** filepath) {
   int i;
   char* tok;
   char* toks[NUM_REQ_DECL_PARTS];

   //separate out the request declaration into tokens
   for (i = 0, tok = strtok(request->buffer, " \n");
        tok;
        i++, tok = strtok(NULL, " \n")) {
      if (i > NUM_REQ_DECL_PARTS - 1) {
         debug(DEBUG_WARNING, "Invalid HTTP request declaration\n");
         return BAD_REQUEST; //should result in a 400
      }
      toks[i] = tok;
   }

   if (strcasestr(toks[0], "GET")) {
      request->type = REQUEST_GET;
   } else {
      debug(DEBUG_WARNING, "Method not supported: %s\n", toks[0]);
      return NOT_ALLOWED; //should result in a 405
   }

   *filepath = toks[1];

   //the second part of the declaration says what HTTP version we're using
   if (strcasestr(toks[2], "HTTP/1.1")) {
      request->httpVersion = 1;
   } else if (strcasestr(toks[2], "HTTP/1.0")) {
      request->httpVersion = 0;
   } else if (strcasestr(toks[2], "HTTP/0.9")) {
      request->httpVersion = -1;
   } else {
      debug(DEBUG_WARNING, "Unsupported HTTP protocol version (!).\n");
      return BAD_REQUEST;
   }

   return 0;
}

//the return value of the function indicates whether there were any errors
//all modification is done through pointers
static int make_request_header(struct request* request) {
   char* lineEnd, * tok, * tmp;
   char* lines[MAX_TOKS];
   int i, numToks, res;

   //find the end of the first line of the request
   if ((lineEnd = (char * ) memmem(request->buffer, request->bytesUsed, "\r\n", 2)) == NULL) {
      debug(DEBUG_WARNING, "Can't find HTTP request declaration\n");
      return BAD_REQUEST; //should result in a 400
   }
   *lineEnd = *(lineEnd + 1) = '\0';

   if ((res = parse_request_declaration(request, &tmp))) {
      return res;
   }

   //split the remainder of the request up
   for (i = 0, tok = strtok(lineEnd + 2, "\r\n");
       tok;
       i++, tok = strtok(NULL, "\r\n")) {
      if (i >= MAX_TOKS) {
         debug(DEBUG_WARNING, "Overflowed header lines buffer\n");
         return BUFFER_OVERFLOW; //should result in a 500
      }
      lines[i] = tok;
   }
   numToks = i;

   request->referer = "";
   request->userAgent = "";

   //we really need to change the way we determine keep-alive
   //http/1.1 specifies persistent unless specifically stated otherwise
   for (i = 0; i < numToks; i++) {
      if (strcasecmp(lines[i], "Connection: Keep-Alive") == 0) {
         request->keepAlive = true;
      } else if (strcasestr(lines[i], "Accept-Encoding:")) {
         if (strcasestr(lines[i] + strlen("Accept-Encoding:"), "deflate")) {
            request->acceptDeflate = true;
         }
      } else if (strcasestr(lines[i], "Referer:")) {
         request->referer = lines[i] + strlen("Referer: ");
      } else if (strcasestr(lines[i], "User-Agent:")) {
         request->userAgent = lines[i] + strlen("User Agent: ");
      }
   }
   url_decode(tmp);
   snprintf(request->filepath, NAME_BUF_LEN, "docs%s", tmp);
   return 0;
}

static inline const char* bool_to_string(bool b) {
   return b ? "true" : "false";
}

static char* request_declaration_to_string(const struct request* request) {
   static char str[NAME_BUF_LEN];
   static char* type2String[NUM_HTTP_REQUEST_TYPES] = {
      "GET", "PUT", "POST", "DELETE"
   };

   snprintf(str, NAME_BUF_LEN, "%s %s HTTP/%.1lf",
            type2String[request->type],
            request->filepath + LEN_DOCS,
            request->httpVersion / 10.0 + 1.0);
   return str;
}

//make this into a to_string method some time in the future
static void print_request(struct request* request) {
   debug(DEBUG_INFO, "Request: %s\n", request_declaration_to_string(request));
   debug(DEBUG_INFO, "|-HTTP-Option Referrer: %s\n", request->referer);
   debug(DEBUG_INFO, "|-HTTP-Option User-Agent: %s\n", request->userAgent);
   debug(DEBUG_INFO, "|-HTTP-Option `Connection: Keep-Alive`?: %s\n",
         bool_to_string(request->keepAlive));
   debug(DEBUG_INFO, "|-HTTP-Option `Accept-Encoding: deflate`?: %s\n",
         bool_to_string(request->keepAlive));
}

//quick and dirty url decoding function. improve later.
//(because it's pretty bad right now)
static void url_decode(char* url) {
   char* tmp = (char *) calloc(1, strlen(url) + 1);
   char hexBuf[3];
   int hexValue;
   int len = strlen(url);
   char* urlPtr = url, * tmpPtr = tmp;

   hexBuf[2] = '\0';

   while (urlPtr - url < len) {
      *tmpPtr = *urlPtr;
      if (*urlPtr == '%') {
         hexBuf[0] = urlPtr[1];
         hexBuf[1] = urlPtr[2];
         hexValue = strtol(hexBuf, NULL, 16);
         *tmpPtr = hexValue;
         urlPtr += 2;
      }
      urlPtr++;
      tmpPtr++;
   }

   strcpy(url, tmp);
   free(tmp);
}

//[requesting host] -- [[time]] "[request line]"
//[http code] [filesize] [referer] [user agent]
static void log_access(struct connection* cxn) {
   static int responseType2Code[NUM_HTTP_RESPONSE_TYPES] = {
      200, 400, 405, 403, 404, 500
   };
   struct sockaddr_in6 me;
   socklen_t sockSize = sizeof(struct sockaddr_in6);
   char timeBuf[NAME_BUF_LEN], v6buf[INET6_ADDRSTRLEN];
   time_t t = time(NULL);

   strftime(timeBuf, NAME_BUF_LEN, "%d/%b/%Y:%X %z", localtime(&t));
   if (getsockname(cxn->socket, (struct sockaddr *) &me, &sockSize)) {
      debug(DEBUG_WARNING, "Error getting sock name\n");
   }

   fprintf(accessLog, "%s - - [%s] \"%s\" %d %u \"%s\" \"%s\"\n",
           inet_ntop(AF_INET6, &me.sin6_addr, v6buf, INET6_ADDRSTRLEN),
           timeBuf,
           request_declaration_to_string(&cxn->request),
           responseType2Code[cxn->response.type],
           cxn->response.contentLength,
           cxn->request.referer,
           cxn->request.userAgent);
}

// In pipes 0 is for writing, 1 is for reading
static int cgi_request(struct env* env, enum CGI_CMD* cmd) {
   socklen_t sockSize = sizeof(struct sockaddr_in6);
   struct sockaddr_in6 me;
   struct sockaddr_in6 server;
   char* pathEnd, * envvarSpace;
   char v6buf[INET6_ADDRSTRLEN];
   int len, i;
   bool hasParams = false;

   if (strstr(env->query + LEN_CGI_BIN, "quit")) {
      *cmd = strcmp(env->query + strlen("/cgi-bin/quit"), "?confirm=1") ?
         CGI_BADPARAMS : CGI_GOODBYE;
      return 0;
   }

   if (strcmp(env->query + LEN_CGI_BIN, "status") == 0) {
      debug(DEBUG_INFO, "cgi status requested\n");
      *cmd = CGI_STATUS;
      return 0;
   }

   if (strcmp(env->query, "/cgi-bin/") == 0) {
      return FILE_NOT_FOUND;
   }

   len = strlen(env->query) - LEN_CGI_BIN;
   if ((pathEnd = strchr(env->query, '?'))) {
      len -= (len - (pathEnd - (env->query + LEN_CGI_BIN)));
      hasParams = true;
   }

   env->filepath = (char *) calloc(1, len + LEN_CGI + 1);
   if (env->filepath == NULL) {
      debug(DEBUG_WARNING, "Failed to allocate cgi filepath\n");
      return POSIX_ERROR;
   }

   memcpy(env->filepath + LEN_CGI, env->query + LEN_CGI_BIN, len);
   (env->filepath + LEN_CGI)[len] = '\0';
   memcpy(env->filepath, "cgi/", LEN_CGI);
   debug(DEBUG_INFO, "cgi filepath: %s\n", env->filepath);

   if (access(env->filepath, X_OK)) {
      free(env->filepath);
      switch(errno) {
      case ENOENT: return FILE_NOT_FOUND;
      case EACCES: return errno;
      default: return POSIX_ERROR;
      }
   }

   if (getsockname(env->serverSocket, (struct sockaddr *) &server, &sockSize) ||
      getsockname(env->mySocket, (struct sockaddr *) &me, &sockSize)) {
      debug(DEBUG_WARNING, "Error getting server socket name\n");
      free(env->filepath);
      return POSIX_ERROR;
   }

   envvarSpace = (char *) calloc(NUM_ENV_VARS, ENV_BUF_LEN);
   if (envvarSpace == NULL) {
      debug(DEBUG_WARNING, "Allocating envvar space failed\n");
      free(env->filepath);
      return POSIX_ERROR;
   }

   for (i = 0; i < NUM_ENV_VARS; i++) {
      env->envvars[i] = envvarSpace + i * ENV_BUF_LEN;
   }

   strcpy(env->envvars[0], "GATEWAY_INTERFACE=CGI/1.1");
   snprintf(env->envvars[1], ENV_BUF_LEN, "QUERY_STRING=%s",
            hasParams ? env->query + LEN_CGI_BIN + len + 1 : "");

   snprintf(env->envvars[2], ENV_BUF_LEN, "REMOTE_ADDR=[%s]",
            inet_ntop(AF_INET6, &me.sin6_addr, v6buf, INET6_ADDRSTRLEN));

   snprintf(env->envvars[3], ENV_BUF_LEN, "REQUEST_METHOD=%s",
            env->method);

   snprintf(env->envvars[4], ENV_BUF_LEN, "SCRIPT_NAME=/cgi-bin/%s",
            env->filepath + LEN_CGI);

   snprintf(env->envvars[5], ENV_BUF_LEN, "SERVER_NAME=[%s]",
            inet_ntop(AF_INET6, &server.sin6_addr, v6buf, INET6_ADDRSTRLEN));

   snprintf(env->envvars[6], ENV_BUF_LEN, "SERVER_PORT=%d",
            ntohs(server.sin6_port));

   strcpy(env->envvars[7], "SERVER_PROTOCOL=HTTP/1.1");
   env->envvars[8] = NULL;

   *cmd = CGI_DO;
   return 0;
}

static int do_cgi(struct env* env) {
   debug(DEBUG_INFO, "CGI operation in progress\n");

   if ((env->childPid = fork()) == -1) {
      debug(DEBUG_WARNING, "fork: %s\n", strerror(errno));
      free(env->filepath);
      return POSIX_ERROR;
   } else if (env->childPid == 0) { //the child
      fcntl(env->mySocket, F_SETFL,
            fcntl(env->mySocket, F_GETFL) & ~O_NONBLOCK);
      dup2(env->mySocket, 1);

      execle(env->filepath, env->filepath, NULL, env->envvars);
      debug(DEBUG_WARNING, "Exec: %s\n", strerror(errno));
      exit(EXIT_FAILURE);
   }

   free(env->filepath);
   free(env->envvars[0]);

   return 0;
}

static inline void set_debug_level(uintptr_t debugLevel) {
   debug(DEBUG_SET, (char *) debugLevel);
}

static void debug(uint32_t debug, const char* msg, ...) {
   static const char* level2String[NUM_DEBUG_LEVELS] = {
      "ERROR", "WARNING", "INFO"
   };
   static uintptr_t debugLevel = DEBUG_INFO;
   static char timeBuf[NAME_BUF_LEN];

   time_t t = time(NULL);
   va_list args;

   if (debug == DEBUG_SET) {
      debugLevel = (intptr_t) msg;
      return;
   }

   if (debug <= debugLevel) {
      strftime(timeBuf, NAME_BUF_LEN, "[%d/%b/%Y:%X %z] - ", localtime(&t));
      fprintf(stderr, "%s%s ", timeBuf, level2String[debug]);
      va_start(args, msg);
      vfprintf(stderr, msg, args);
      va_end(args);
   }
}

static void clean_exit(int unused) {
   connections_t::iterator itr;

   fdarr_cntl(FDARR_CLEAN_UP);
   fclose(accessLog);

   for (itr = connections->begin(); itr != connections->end(); itr++) {
      struct connection* cxn = itr->second;
      if (cxn) {
         if (connections->count(cxn->file)) {
            connections->at(cxn->file) = NULL;
            munmap(cxn->fileBuf, cxn->response.contentLength);
            close(cxn->file);
         }
         close(cxn->socket);
         debug(DEBUG_INFO, "Freeing connections struct\n");
         free(cxn);
      }
   }
   delete connections;
   delete ext2mime;
   printf("Server exiting cleanly.\n");
   exit(EXIT_SUCCESS);
}

static void wait_cgi(int unused) {
   int childStatus;

   debug(DEBUG_INFO, "Child signal received\n");

   if (waitpid(0, &childStatus, WNOHANG)) {
      if (WIFEXITED(childStatus)) {
         debug(DEBUG_INFO, "Child exits\n");
      }
   }
}

//sets up a signal handler
static void add_handler(int signal, void (*handlerFunc)(int)) {
   struct sigaction sa;

   sa.sa_handler = handlerFunc;
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = 0;
   if (sigaction(signal, &sa, NULL) < 0)  {
      pexit("Adding sighandler failed");
   }
}

static inline void pexit(const char* str) {
   debug(DEBUG_ERROR, "POSIX %s: %s\n", str, strerror(errno));
   exit(EXIT_FAILURE);
}
