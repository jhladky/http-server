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

   set_debug_level(INFO);
   //set_debug_level(DEBUG_NONE);
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
         debug(WARNING, "Converting v4 to v6 addr\n");
         snprintf(v6buf, INET6_ADDRSTRLEN, "::ffff:%s", argv[1]);
         if (inet_pton(AF_INET6, v6buf, &me.sin6_addr) == 0) {
            debug(ERROR, "Invalid address.\n");
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

   if (fdarr_cntl(ADD, mySock, POLLIN) == MALLOC_FAILED) {
      debug(ERROR, "malloc failed\n");
      exit(EXIT_FAILURE);
   }

   //the event loop
   while (true) {
      struct pollfd* fds;
      nfds_t nfds;
      unsigned int i;
      int res, fd;

      fdarr_cntl(GET, &fds, &nfds);
      if ((res = poll(fds, nfds, -1)) == -1) {
         debug(WARNING, "Poll: %s\n", strerror(errno));
         continue; //not a fan of this
      }

      for (i = 0; i < nfds; i++) {
         fd = fds[i].fd;

         if (fd != mySock && connections->count(fd) == 0) {
            debug(ERROR, "Unrecognized fd(%d) in fdarr\n", fd);
            exit(EXIT_FAILURE);
         }

         if (fd == mySock) {
            res = process_mysock_events(mySock, fds[i].revents);
         } else if (fd == connections->at(fd)->socket) {
            res = process_cxsock_events(connections->at(fd), fds[i].revents);
         } else if (fd == connections->at(fd)->file) {
            res = process_cxfile_events(connections->at(fd), fds[i].revents);
         }

         //if  the fdarr was modified, break from the loop
         //and start over completely with the right nfds value
         if (res == FDARR_MODIFIED) {
            debug(INFO, "Fdarr modified, breaking\n");
            break;
         } else if (res == BAD_SOCKET) {
            debug(WARNING, "Error accepting socket connection\n");
         } else if (res == MALLOC_FAILED) {
            debug(ERROR, "Allocating connections struct failed\n");
            exit(EXIT_FAILURE);
         }
      }
   }  //should close mySock somewhere... ehhhhh

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
      debug(INFO, "mysock, revent: POLLIN, state: N/A\n");
      cxn = (struct connection *) calloc(1, sizeof(struct connection)); //(1)
      if (cxn == NULL) {
         return MALLOC_FAILED;
      }

      cxn->socket = accept(socket, (struct sockaddr *) &me, &sockSize);
      if (cxn->socket == -1) {
         perror("mysock accept");
         return BAD_SOCKET; //(2)
      }

      cxn->state = REQUEST_INP;
      cxn->response.httpCode = HTTP_OK;
      cxn->env.mySocket = cxn->socket;
      cxn->env.serverSocket = socket;
      debug(INFO, "Connected to  %s\n",
            inet_ntop(AF_INET6, &me.sin6_addr, v6buf, INET6_ADDRSTRLEN));
      numClients++;

      fdarr_cntl(ADD, cxn->socket, POLLIN | POLLOUT);
      err = FDARR_MODIFIED;
      connections->insert(std::make_pair(cxn->socket, cxn));
      debug(INFO, "Allocated connection struct(%d)\n", cxn->socket);
   }

   return err;
}

static int process_cxsock_events(struct connection* cxn, short revents) {
   int res, err = 0;
   char* ptr;

   if (revents & POLLIN && cxn->state == REQUEST_INP) {
      debug(INFO, "cxsock, revent: POLLIN, state: REQUEST_INP\n");

      switch(res = fill_request_buffer(cxn->socket, &cxn->request)) {
      case ZERO_READ:
         debug(INFO, "Zero read, closing connection\n");
         close_connection(cxn);
         err = FDARR_MODIFIED;
         break;
      case BUFFER_OVERFLOW:
         //fix this for real-world later
         cxn->state = INTERNAL;
         debug(WARNING, "Overflowed request header buffer\n");
         internal_response(&cxn->response, HTTP_INTERNAL);
         break;
      case 0:
         numRequests++;
         res = process_request(cxn);

         if (res == HTTP_CGI) {
            cxn->cgi = true;
            cxn->state = INTERNAL;
         } else if (res == CGI_QUIT) {
            cxn->quit = true;
            cxn->state = INTERNAL;
         } else if (res == INTERNAL_RESPONSE) {
            cxn->state = INTERNAL;
         } else {
            cxn->fileBuf = (char *) mmap(NULL, cxn->response.contentLength,
                                         PROT_READ, MAP_PRIVATE, cxn->file, 0);
            if ((intptr_t) cxn->fileBuf == -1) {
               cxn->state = INTERNAL;
               perror("mmap");
               close(cxn->file);
               internal_response(&cxn->response, HTTP_INTERNAL);
            } else {
               cxn->state = RESPONSE_HEA;
               fdarr_cntl(ADD, cxn->file, POLLIN);
               err = FDARR_MODIFIED;
               connections->insert(std::make_pair(cxn->file, cxn));
               debug(INFO, "Adding fd to struct, #%d\n", cxn->file);
               ptr = strchr(cxn->request.filepath, '.');
               if (ext2mime->count(std::string(ptr + 1))) {
                  cxn->response.contentType =
                     ext2mime->at(std::string(ptr +1)).c_str();
               }

               make_response_header(&cxn->response);
            }
         }
         break;
      default:
         break;
      }
   } else if (revents & POLLOUT && cxn->state == INTERNAL) {
      debug(INFO, "cxsock, revent: POLLOUT, state: RESPONSE_HEA\n");

      res = write(cxn->socket,
                  cxn->response.buffer,
                  cxn->response.bytesToWrite);
      cxn->state = cxn->cgi ? CGI_FIL : RESPONSE_FIN;
   } else if (revents & POLLOUT && cxn->state == RESPONSE_HEA) {
      debug(INFO, "cxsock, revent: POLLOUT, state: RESPONSE_HEA\n");

      res = write(cxn->socket,
                  cxn->response.buffer,
                  cxn->response.headerLength);
      cxn->response.bytesToWrite -= res;
      cxn->state = res == -1 ? RESPONSE_FIN : RESPONSE_FIL;
   } else if (revents & POLLOUT && cxn->state == RESPONSE_FIL) {
      debug(INFO, "cxsock, revent: POLLOUT, state: RESPONSE_INP\n");
      ptr = cxn->fileBuf + cxn->response.contentLength -
         cxn->response.bytesToWrite;
      res = write(cxn->socket,
                  ptr,
                  cxn->response.bytesToWrite);

      if (res == -1) {
         cxn->state = RESPONSE_FIN;
      } else {
         cxn->response.bytesToWrite -= res;
         cxn->state = cxn->response.bytesToWrite ? LOAD_FILE : RESPONSE_FIN;
      }
   } else if (revents & POLLOUT && cxn->state == CGI_FIL) {
      if (do_cgi(&cxn->env) == HTTP_INTERNAL) {
         //internal server error here
         //we'd need to go backwards in the state machine
      }
      cxn->response.httpCode = HTTP_OK;
      cxn->state = RESPONSE_FIN;
      return process_cxsock_events(cxn, POLLOUT); //fix me later
   } else if (revents & POLLOUT && cxn->state == RESPONSE_FIN) {
      debug(INFO, "cxsock, revent: POLLOUT, state: RESPONSE_FIN\n");

      log_access(cxn);

      if (cxn->quit) {
         clean_exit(0);
      } else if (cxn->request.keepAlive && cxn->response.httpCode == HTTP_OK) {
         reset_connection(cxn);
         cxn->state = REQUEST_INP;
      } else {
         debug(INFO, "Keep alive not specified or "
               "http error encountered; Closing connection\n");
         close_connection(cxn);
      }
      err = FDARR_MODIFIED;
   } else if (revents & POLLHUP || revents & POLLERR || revents & POLLNVAL) {
      debug(INFO, "cxsock, revent: POLLHUP/POLLERR/POLLNVAL\n");
      close_connection(cxn);
      err = FDARR_MODIFIED;
   }

   return err;
}

//(1) We don't need to do anything, this is just a
//    condition to be able to write back onto the socket
static int process_cxfile_events(struct connection* cxn, short revents) {
   int err = 0;

   if (revents & POLLIN && cxn->state == LOAD_FILE) {
      debug(INFO, "cxfile, revent: POLLIN, state: LOAD_FILE\n");
      cxn->state = RESPONSE_FIL; //(1)
   }

   return err;
}

static int process_request(struct connection* cxn) {
   struct env* env = &cxn->env;
   struct request* request = &cxn->request;
   struct response* response = &cxn->response;
   int res;

   debug(INFO, "Processing request buffer\n");
   res = make_request_header(env, request);

   response->usingDeflate = request->acceptDeflate;
   response->contentLength = request->contentLength;

   //hacked this in for JSON
   if (res == JSON_QUIT) {
      json_response(response, res);
      return CGI_QUIT;
   } if (res >= JSON_ABOUT && res <= JSON_FORTUNE) {
      json_response(response, res);
      return INTERNAL_RESPONSE;
   }

   switch(res) {
   case HTTP_CGI:
      debug(INFO, "Making CGI response header\n");
      internal_response(response, res);
      return HTTP_CGI;
   case CGI_QUIT:
      debug(INFO, "Received quit, starting exit process\n");
      internal_response(response, HTTP_GOODBYE);
      return CGI_QUIT;
   case HTTP_NOTFOUND:
   case HTTP_DENIED: //make the non-cgi open do this as well
   case HTTP_CGISTATUS:
   case HTTP_BADPARAMS:
      internal_response(response, res);
      return INTERNAL_RESPONSE;
   case HTTP_GENLIST:
      if (generate_listing(request->filepath, response)) {
         debug(WARNING, "Generate listing error\n");
         internal_response(response, HTTP_INTERNAL);
      }
      return INTERNAL_RESPONSE;
   default:
   case BAD_REQUEST:
   case BUFFER_OVERFLOW:
   case MALLOC_FAILED:
      debug(WARNING, "Malloc failed, buffer overflow, "
            "or bad request\n");
      internal_response(response, HTTP_INTERNAL);
      return INTERNAL_RESPONSE;
   case 0:
      break;
   }

   res = open(request->filepath, O_RDONLY | O_NONBLOCK);
   if (res  == -1 && errno == EACCES) {
      internal_response(response, HTTP_DENIED);
      return INTERNAL_RESPONSE;
   }

   if (res == -1) {
      perror("open");
      internal_response(response, HTTP_INTERNAL);
      return INTERNAL_RESPONSE;
   }

   cxn->file = res;
   return 0;
}

static void reset_connection(struct connection* cxn) {
   int socket = cxn->socket;
   struct env env;

   debug(INFO, "Resetting connection\n");
   env = cxn->env;
   connections->erase(cxn->file);
   munmap(cxn->fileBuf, cxn->response.contentLength);
   debug(INFO, "Freeing filepath\n");
   if (cxn->request.filepath) {
      free(cxn->request.filepath);
   }
   free(cxn->request.line);
   fdarr_cntl(REMOVE, cxn->file);
   close(cxn->file);
   memset(cxn, 0, sizeof(struct connection));
   cxn->socket = socket;
   cxn->env = env;
   cxn->response.httpCode = HTTP_OK;
}

static void close_connection(struct connection* cxn) {
   debug(INFO, "Closing connection...\n");

   fdarr_cntl(REMOVE, cxn->socket);
   if (cxn->file) {
      fdarr_cntl(REMOVE, cxn->file);
      connections->erase(cxn->file);
      close(cxn->file);
   }
   if (cxn->request.filepath) { ///THIS IS BAD FIXFIXFIX
      free(cxn->request.filepath);
   }
   if (cxn->request.line) {
      free(cxn->request.line);
   }
   close(cxn->socket);
   connections->erase(cxn->socket);
   debug(INFO, "Freeing connection struct\n");
   free(cxn);
}

static int fill_request_buffer(int socket, struct request* request) {
   int bytesRead = 0;

   if ((bytesRead = read(socket,
                        request->buffer + request->bytesUsed,
                        BUF_LEN - request->bytesUsed)) == -1) {

      perror("socket read");
      return UNRECOVERABLE;
   } else if (bytesRead == 0) {
      debug(WARNING, "Zero read from socket\n");
      return ZERO_READ;
   }

   request->bytesUsed += bytesRead;

   if (request->bytesUsed >= BUF_LEN) {
      debug(WARNING, "Overflowed header buffer\n");
      return BUFFER_OVERFLOW;
   }

   if (memmem(request->buffer, request->bytesUsed,
             "\r\n\r\n", strlen("\r\n\r\n")) == NULL) {
      debug(INFO, "Request incomplete\n");
      return REQUEST_INCOMPLETE;
   }

   debug(INFO, "End of request found\n");

   return 0;
}

static int fdarr_cntl(enum CMD cmd, ...) {
   static struct pollfd* fds = NULL;
   static nfds_t nfds = 0;
   static unsigned int fdarrMaxSize = 50;
   struct pollfd** fdsFill;
   nfds_t* nfdsFill;
   unsigned int i;
   int fd, err;
   va_list args;

   va_start(args, cmd);
   if (nfds >= fdarrMaxSize - 1 || fds == NULL) {
      fdarrMaxSize *= 2;
      debug(INFO, "Expanding pollfd array from "
            "size %d to size %d\n", nfds, fdarrMaxSize);
      fds = (struct pollfd *) realloc(fds,
                                      fdarrMaxSize * sizeof(struct pollfd));
      if (fds == NULL) {
         debug(WARNING, "Allocating pollfd array failed\n");
         return MALLOC_FAILED;
      }
      err = STRUCT_SIZE_CHANGE;
   }

   switch(cmd) {
   default:
   case GET:
      fdsFill = va_arg(args, struct pollfd **);
      nfdsFill = va_arg(args, nfds_t *);

      *fdsFill = fds;
      *nfdsFill = nfds;
      break;
   case ADD:
      fds[nfds].fd = va_arg(args, int);
      debug(INFO, "Adding %dth entry(%d) to "
            "pollfd array\n", nfds, fds[nfds].fd);
      fds[nfds].events = (short) va_arg(args, int);
      nfds++;
      break;
   case MODIFY:
      fd = va_arg(args, int);
      for (i = 0; i < nfds; i++) {
         if (fds[i].fd == fd) {
            break;
         }
      }

      fds[i].events = (short) va_arg(args, int);
      break;
   case REMOVE:
      fd = va_arg(args, int);
      for (i = 0; i < nfds; i++) {
         if (fds[i].fd == fd) {
            break;
         }
      }

      if (i == nfds) {
         debug(WARNING, "Entry not found in pollfd array\n");
         return STRUCT_NOT_FOUND;
      }

      debug(INFO, "Removing %dth entry(%d) from "
            "pollfd array\n", i, fd);
      if (i != nfds - 1 && nfds > 1) {
         memcpy(fds + i, fds + nfds - 1, sizeof(struct pollfd));
      }
      nfds--;
      break;
   case CLEAN_UP:
      debug(INFO, "Freeing pollfd array\n");
      free(fds);
      break;
   }

   va_end(args);
   return err;
}

static int json_response(struct response* response, int code) {
   int bytesWritten = 0;
   long memUsage;
   double cpuTime, timeDiff;
   struct rusage usage;
   struct timeval now;
   char buf[BUF_LEN];

   switch(code) {
   case JSON_IMPLEMENTED:
      debug(INFO, "JSON: doing implemented.json\n");
      bytesWritten = sprintf(buf, "%s", BODY_JSON_IMPLEMENTED_1);
      break;
   case JSON_ABOUT:
      debug(INFO, "JSON: doing about.json\n");
      bytesWritten = sprintf(buf, "%s", BODY_JSON_ABOUT);
      break;
   case JSON_QUIT:
      debug(INFO, "JSON: doing quit\n");
      bytesWritten = sprintf(buf, "%s", BODY_JSON_QUIT);
      break;
   case JSON_STATUS:
      debug(INFO, "JSON: doing status.json\n");
      if (getrusage(RUSAGE_SELF, &usage) == -1 ||
          gettimeofday(&now, NULL) == -1) {
         return internal_response(response, HTTP_INTERNAL);
      }
      memUsage = usage.ru_ixrss + usage.ru_idrss + usage.ru_isrss;
      cpuTime = (usage.ru_utime.tv_sec * USEC_PER_SEC +
                 usage.ru_stime.tv_sec * USEC_PER_SEC +
                 usage.ru_utime.tv_usec +
                 usage.ru_stime.tv_usec) / USEC_PER_SEC;
      timeDiff = ((now.tv_sec * USEC_PER_SEC + now.tv_usec) -
                  (startTime.tv_sec * USEC_PER_SEC + startTime.tv_usec)) / USEC_PER_SEC;
      bytesWritten = sprintf(buf, BODY_JSON_STATUS, numClients, numRequests,
                             errors, timeDiff, cpuTime, memUsage);
      break;
   default:
   case JSON_FORTUNE:
      debug(INFO, "JSON: doing fortune.json\n");
      break;
   }

   response->httpCode = HTTP_OK;
   response->contentLength = bytesWritten;
   response->contentType = "application/json";

   make_response_header(response);
   memcpy(response->buffer + response->headerLength, buf, bytesWritten);
   response->bytesUsed += bytesWritten;
   return 0;
}

static int internal_response(struct response* response, int code) {
   int realCode = code;
   char buf[BUF_LEN];
   time_t t = time(NULL);
   int bytesWritten = 0;

   switch(code) {
   case HTTP_DENIED:
      errors++;
      debug(INFO, "Sending error 403 Denied\n");
      bytesWritten = sprintf(buf, "%s", BODY_403);
      break;
   case HTTP_NOTFOUND:
      errors++;
      debug(INFO, "Sending error 404 Not Found\n");
      bytesWritten = sprintf(buf, "%s", BODY_404);
      break;
   case HTTP_BADPARAMS:
      errors++;
      debug(INFO, "Sending Bad Params message\n");
      bytesWritten = sprintf(buf, "Bad Params!");
      code = HTTP_OK;
      break;
   case HTTP_GOODBYE:
      debug(INFO, "Sending Goodbye message\n");
      bytesWritten = sprintf(buf, "Goodbye!");
      code = HTTP_OK;
      break;
   case HTTP_CGISTATUS:
      debug(INFO, "Sending CGI status message\n");
      bytesWritten = sprintf(
         buf, "%sServer Process ID: %d<BR>\nCurrent Time: %s%s",
         BODY_STATUS_BEGIN, getpid(), asctime(localtime(&t)), BODY_STATUS_END);
      code = HTTP_OK;
      break;
   case HTTP_CGI:
      debug(INFO, "Sending CGI response line\n");
      bytesWritten = sprintf(response->buffer, "HTTP/1.1 200 OK\r\n");
      response->httpCode = HTTP_OK;
      response->bytesToWrite = response->bytesUsed =
         response->headerLength = bytesWritten;
      return 0;
   default:
   case HTTP_INTERNAL:
      errors++;
      debug(WARNING, "Sending error 500 Internal\n");
      bytesWritten = sprintf(buf, "%s", BODY_500);
      break;
   }

   response->httpCode = code;
   response->contentLength = bytesWritten;
   response->contentType = (realCode == HTTP_BADPARAMS ||
                            realCode == HTTP_GOODBYE) ?
      "text/plain" : "text/html";

   make_response_header(response);
   memcpy(response->buffer + response->headerLength, buf, bytesWritten);
   response->bytesUsed += bytesWritten;
   return 0;
}

//overloaded initally
static int make_response_header(struct response* response) {
   int written = 0;

   switch(response->httpCode) {
   case HTTP_OK:
      written = sprintf(response->buffer, "HTTP/1.1 200 OK\r\n");
      break;
   case HTTP_NOTFOUND:
      written = sprintf(response->buffer, "HTTP/1.1 404 Not Found\r\n");
      break;
   case HTTP_DENIED:
      written = sprintf(response->buffer, "HTTP/1.1 403 Forbidden\r\n");
      break;
   default:
   case HTTP_INTERNAL:
      written = sprintf(response->buffer,
                        "HTTP/1.1 500 Internal Server Error\r\n");
      break;
   }

   if (response->contentType) {
      written += sprintf(response->buffer + written,
                         "Content-Type: %s\r\n", response->contentType);
   }

   if (response->usingDeflate) {
      written += sprintf(response->buffer + written,
                         "Content-Encoding: deflate\r\n");
   } else {
      written += sprintf(response->buffer + written,
                         "Content-Length: %d\r\n\r\n", response->contentLength);
   }
   response->bytesUsed = response->headerLength = written;
   response->bytesToWrite = response->headerLength + response->contentLength;
   return 0;
}

static int generate_listing(char* filepath, struct response* response) {
   char* buf;
   //int bytesWritten = 0, bufLen, numDirEntries = 0;
   int bytesWritten = 0, numDirEntries = 0;
   DIR* parent;
   char* str = (char *) calloc(1, strlen(filepath) - LEN_DOCS + 1);
   struct dirent* entry;

   debug(INFO, "In generate_listing\n");

   if (str == NULL) {
      debug(WARNING, "Allocating filepath copy failed\n");
      return MALLOC_FAILED;
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
      debug(WARNING, "Allocating generate listing buffer failed\n");
      free(str);
      return MALLOC_FAILED;
   }
   //bufLen = AVG_LISTING_LEN * numDirEntries;

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
   response->httpCode = 200;
   response->contentLength = bytesWritten;

   make_response_header(response);
   memcpy(response->buffer + response->headerLength, buf, bytesWritten);
   response->bytesUsed += bytesWritten;

   free(str);
   free(buf);
   return 0;
}

//the return value of the function indicates whether there were any errors
//all modification is done through pointers
static int make_request_header(struct env* env, struct request* request) {
   char* buf = request->buffer;
   struct stat stats;
   //char* lineEnd, * tok, * loc;
   char* lineEnd, * tok;
   char* toks[MAX_TOKS], * lines[MAX_TOKS];
   int i, numToks;

   lineEnd = (char * ) memmem(request->buffer, request->bytesUsed, "\r\n", 2);
   if (lineEnd == NULL) {
      debug(WARNING, "Error processing request\n");
      return BAD_REQUEST;
   }

   *lineEnd = *(lineEnd + 1) = '\0';
   debug(INFO, "Request: %s\n", buf);

   request->line = (char *) calloc(1, strlen(request->buffer) + 1);
   if (request->line == NULL) {
      debug(WARNING, "Allocating request line failed\n");
      return MALLOC_FAILED;
   }

   strcpy(request->line, request->buffer);

   for (i = 0, tok = strtok(buf + strlen(buf) + 2, "\r\n");
       tok;
       i++, tok = strtok(NULL, "\r\n")) {
      if (i >= MAX_TOKS) {
         debug(WARNING, "Overflowed header lines buffer\n");
         return BUFFER_OVERFLOW;
      }
      lines[i] = tok;
   }
   numToks = i;

   for (i = 0, tok = strtok(buf, " \n"); tok; i++, tok = strtok(NULL, " \n")) {
      if (i >= MAX_TOKS) {
         debug(WARNING, "Overflowed header lines buffer\n");
         return BUFFER_OVERFLOW;
      }
      toks[i] = tok;
   }

   //we really need to change the way we determine keep-alive
   //http/1.1 specifies persistent unless specifically stated otherwise
   for (i = 0; i < numToks; i++) {
      if (strcasecmp(lines[i], "Connection: keep-alive") == 0) {
         debug(INFO, "* HTTP Option: %s\n", lines[i]);
         request->keepAlive = true;
      } else if (strcasestr(lines[i], "Accept-Encoding:")) {
         if (strcasestr(lines[i] + strlen("Accept-Encoding:"), "deflate")) {
            debug(INFO, "* HTTP Option: %s\n", lines[i]);
            request->acceptDeflate = true;
         }
      } else if (strcasestr(lines[i], "Referer:")) {
         debug(INFO, "* HTTP Option: %s\n", lines[i]);
         request->referer = lines[i] + strlen("Referer: ");
      } else if (strcasestr(lines[i], "User-Agent:")) {
         debug(INFO, "* HTTP Option: %s\n", lines[i]);
         request->userAgent = lines[i] + strlen("User Agent: ");
      }
   }

   if (request->referer == NULL) {
      request->referer = "";
   }

   if (request->userAgent == NULL) {
      request->userAgent = "";
   }

   url_decode(toks[1]);

   if (strcmp(toks[0], "GET")) {
      debug(WARNING, "Server does not support operation\n");
      return HTTP_NOTFOUND; //this is what he does...no idea why
   }

   if ((strstr(toks[1], "/cgi-bin/"))) { //check for cgi
      debug(INFO, "cgi request: %s\n", toks[1]);
      env->method = toks[0];
      env->query = toks[1];
      return cgi_request(env);
   }

   if (strstr(toks[1], "/json/")) { //check for json
      debug(INFO, "json request: %s\n", toks[1]);
      env->method = toks[0];
      env->query = toks[1];
      return json_request(env);
   }

   //one extra char is for the null btye
   //the other is for the possible '/' we might have to add
   request->filepath = (char *) calloc(1, strlen(toks[1]) +
                                       LEN_DOCS + LEN_INDEX_HTML + 2);
   if (request->filepath == NULL) {
      debug(WARNING, "Allocating filepath failed\n");
      return MALLOC_FAILED;
   }

   strcpy(request->filepath, "docs");
   strcpy(request->filepath + strlen("docs"), toks[1]);

   if (stat(request->filepath, &stats)) {
      debug(INFO, "File not found... generating 404\n");
      return HTTP_NOTFOUND; //this is a genuine 404
   }

   if (S_ISDIR(stats.st_mode)) {
      if (request->filepath[strlen(request->filepath) - 1] != '/') {
         strcat(request->filepath, "/");
      }
      strcat(request->filepath, "index.html");

      //maybe change this to an access() call
      if (stat(request->filepath, &stats)) {
         debug(INFO, "Generating directory listing\n");
         //this is where we generate the listing
         return HTTP_GENLIST;
      }
   }

   request->contentLength = stats.st_size;
   debug(INFO, "File{size: %d, "
         "path: %s}\n", stats.st_size, request->filepath);
   return 0;
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
   struct sockaddr_in6 me;
   socklen_t sockSize = sizeof(struct sockaddr_in6);
   char timeBuf[NAME_BUF_LEN], v6buf[INET6_ADDRSTRLEN];
   time_t t = time(NULL);

   if (strftime(timeBuf, NAME_BUF_LEN, "%d/%b/%Y:%X %z", localtime(&t)) == 0) {
      debug(WARNING, "Error formatting log string\n");
   }

   if (getsockname(cxn->socket, (struct sockaddr *) &me, &sockSize)) {
      debug(WARNING, "Error getting sock name\n");
   }

   fprintf(accessLog, "%s - - [%s] \"%s\" %d %u \"%s\" \"%s\"\n",
           inet_ntop(AF_INET6, &me.sin6_addr, v6buf, INET6_ADDRSTRLEN),
           timeBuf,
           cxn->request.line,
           cxn->response.httpCode,
           cxn->response.contentLength,
           cxn->request.referer,
           cxn->request.userAgent);
}

/*implemented.json - Returns a list of implemented features and associated URLs • about - Returns information about the author of the program
• quit - Causes the server to quit
• status - Returns server usage statistics
*/

static int json_request(struct env* env) {
   if (strcmp(env->query, "/json/") == 0) {
      return HTTP_NOTFOUND;
   } else if (strcmp(env->query + LEN_JSON, "quit") == 0) {
      return JSON_QUIT;
   } else if (strcmp(env->query + LEN_JSON, "about.json") == 0) {
      return JSON_ABOUT;
   } else if (strcmp(env->query + LEN_JSON, "implemented.json") == 0) {
      return JSON_IMPLEMENTED;
   } else if (strcmp(env->query + LEN_JSON, "status.json") == 0) {
      return JSON_STATUS;
   } else if (strcmp(env->query + LEN_JSON, "fortune.json") == 0) {
      return JSON_FORTUNE;
   }

   debug(WARNING, "Unsupported JSON operation requested");
   //return DO_JSON;
   return HTTP_NOTFOUND;
}

// In pipes 0 is for writing, 1 is for reading
static int cgi_request(struct env* env) {
   socklen_t sockSize = sizeof(struct sockaddr_in6);
   struct sockaddr_in6 me;
   struct sockaddr_in6 server;
   char* pathEnd, * envvarSpace;
   char v6buf[INET6_ADDRSTRLEN];
   int len, i;
   bool hasParams = false;

   if (strstr(env->query + LEN_CGI_BIN, "quit")) {
      return strcmp(env->query + strlen("/cgi-bin/quit"), "?confirm=1") ?
         HTTP_BADPARAMS : CGI_QUIT;
   }

   if (strcmp(env->query + LEN_CGI_BIN, "status") == 0) {
      debug(INFO, "cgi status requested\n");
      return HTTP_CGISTATUS;
   }

   if (strcmp(env->query, "/cgi-bin/") == 0) {
      return HTTP_NOTFOUND;
   }

   len = strlen(env->query) - LEN_CGI_BIN;
   if ((pathEnd = strchr(env->query, '?'))) {
      len -= (len - (pathEnd - (env->query + LEN_CGI_BIN)));
      hasParams = true;
   }

   env->filepath = (char *) calloc(1, len + LEN_CGI + 1);
   if (env->filepath == NULL) {
      debug(WARNING, "Failed to allocate cgi filepath\n");
      return MALLOC_FAILED;
   }

   memcpy(env->filepath + LEN_CGI, env->query + LEN_CGI_BIN, len);
   (env->filepath + LEN_CGI)[len] = '\0';
   memcpy(env->filepath, "cgi/", LEN_CGI);
   debug(INFO, "cgi filepath: %s\n", env->filepath);

   if (access(env->filepath, X_OK)) {
      free(env->filepath);
      switch(errno) {
      case ENOENT: return HTTP_NOTFOUND;
      case EACCES: return HTTP_DENIED;
      default: return HTTP_INTERNAL;
      }
   }

   if (getsockname(env->serverSocket, (struct sockaddr *) &server, &sockSize) ||
      getsockname(env->mySocket, (struct sockaddr *) &me, &sockSize)) {
      debug(WARNING, "Error getting server socket name\n");
      free(env->filepath);
      return HTTP_INTERNAL;
   }

   envvarSpace = (char *) calloc(NUM_ENV_VARS, ENV_BUF_LEN);
   if (envvarSpace == NULL) {
      debug(WARNING, "Allocating envvar space failed\n");
      free(env->filepath);
      return MALLOC_FAILED;
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

   return HTTP_CGI;
}

static int do_cgi(struct env* env) {
   debug(INFO, "CGI operation in progress\n");

   if ((env->childPid = fork()) == -1) {
      debug(WARNING, "fork failed\n");
      free(env->filepath);
      return HTTP_INTERNAL;
   } else if (env->childPid == 0) { //the child
      fcntl(env->mySocket, F_SETFL, fcntl(env->mySocket, F_GETFL) & ~O_NONBLOCK);
      dup2(env->mySocket, 1);

      execle(env->filepath, env->filepath, NULL, env->envvars);
      debug(WARNING, "Exec: %s\n", strerror(errno));
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
   static uintptr_t debugLevel = INFO;
   static char timeBuf[NAME_BUF_LEN];
   time_t t = time(NULL);
   va_list args;

   if (debug == DEBUG_SET) {
      debugLevel = (intptr_t) msg;
      return;
   }

   if (debug <= debugLevel) {
      strftime(timeBuf, NAME_BUF_LEN, "[%d/%b/%Y:%X %z] - ", localtime(&t));
      switch(debug) {
      case ERROR: fprintf(stderr, "%s ERROR ", timeBuf); break;
      case WARNING: fprintf(stderr, "%s WARNING ", timeBuf); break;
      default:
      case INFO: fprintf(stderr, "%s INFO ", timeBuf); break;
      }
      va_start(args, msg);
      vfprintf(stderr, msg, args);
      va_end(args);
   }
}

static void clean_exit(int unused) {
   connections_t::iterator itr;

   fdarr_cntl(CLEAN_UP);
   fclose(accessLog);

   for (itr = connections->begin(); itr != connections->end(); itr++) {
      struct connection* cxn = itr->second;
      if (cxn) {
         free(cxn->request.filepath);
         free(cxn->request.line);
         if (connections->count(cxn->file)) {
            connections->at(cxn->file) = NULL;
            munmap(cxn->fileBuf, cxn->response.contentLength);
            close(cxn->file);
         }
         close(cxn->socket);
         debug(INFO, "Freeing connections struct\n");
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

   debug(INFO, "Child signal received\n");

   if (waitpid(0, &childStatus, WNOHANG)) {
      if (WIFEXITED(childStatus)) {
         debug(INFO, "Child exits\n");
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
   perror(str);
   exit(EXIT_FAILURE);
}
