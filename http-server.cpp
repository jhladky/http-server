#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <netdb.h>
#include <dirent.h>
#include <errno.h>
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

#include "http-server.h"


// Hashtable linking file descriptors (either for a socket, pipe, or file)
// to connection structs.
static connections_t* connections = new connections_t(500);
static FILE* accessLog; // don't like this being global
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
static const char* responseType2String[NUM_HTTP_RESPONSE_TYPES] = {
   "200 OK",
   "400 Bad Request",
   "405 Method Not Allowed",
   "403 Forbidden",
   "404 Not Found",
   "500 Internal Server Error"
};
static const char* requestType2String[NUM_HTTP_REQUEST_TYPES] = {
   "GET",
   "PUT",
   "POST",
   "DELETE"
};
int mySock;

int main(int argc, char* argv[]) {
   struct sockaddr_in6 me;
   int res;
   char v6buf[INET6_ADDRSTRLEN];
   socklen_t sockSize = sizeof(struct sockaddr_in6);

#ifdef DEBUG
   set_debug_level(DEBUG_INFO);
#else
   set_debug_level(DEBUG_NONE);
#endif
   debug(DEBUG_INFO, "Starting on port %d...\n", SERVER_PORT);

   add_handler(SIGINT, clean_exit);
   signal(SIGCHLD, SIG_IGN);

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
   me.sin6_port = htons(SERVER_PORT);
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
      int res, fd, revents;

      fdarr_cntl(FDARR_GET, &fds, &nfds);
      //debug(DEBUG_INFO, ">>> Calling poll.\n");
      if ((res = poll(fds, nfds, -1)) == -1) {
         switch (errno) {
         case EINTR:
            debug(DEBUG_INFO, "Poll interrupted by signal.\n");
            continue;
         default:
            debug(DEBUG_ERROR, "poll: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
         }
      }
      //debug(DEBUG_INFO, "<<< Returning from poll.\n");

      for (i = 0; i < nfds; i++) {
         fd = fds[i].fd;
         revents = fds[i].revents;

         if (fd != mySock && connections->count(fd) == 0) {
            debug(DEBUG_ERROR, "Unrecognized fd(%d) in fdarr\n", fd);
            exit(EXIT_FAILURE);
         }

         if (revents && fd == mySock) {
            res = process_mysock_events(mySock, revents);
         } else if (revents && fd == connections->at(fd)->socket) {
            res = process_cxsock_events(connections->at(fd), revents);
         } else if (revents && fd == connections->at(fd)->file) {
            res = process_cxfile_events(connections->at(fd), revents);
         } else if (revents) {
            debug(DEBUG_WARNING, "Unknown event!\n");
         }

         // If the fdarr was modified, break from the loop
         // and start over completely with the right nfds value.
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

static int process_mysock_events(int socket, short revents) {
   struct connection* cxn;
   struct sockaddr_in6 me;
   socklen_t sockSize = sizeof(struct sockaddr_in6);
   char v6buf[INET6_ADDRSTRLEN];
   int err = 0;

   if (revents & POLLIN) {
      debug(DEBUG_INFO, "mysock, revent: POLLIN, state: N/A\n");
      cxn = (struct connection *) calloc(1, sizeof(struct connection));
      if (cxn == NULL) {
         return POSIX_ERROR;
      }

      cxn->socket = accept(socket, (struct sockaddr *) &me, &sockSize);
      if (cxn->socket == -1) {
         // We can't generate an internal error because we can't
         // communicate with the client, so we just return and hope it
         // doesn't happen again
         debug(DEBUG_WARNING, "accept: %s\n", strerror(errno));
         return BAD_SOCKET;
      }

      cxn->env.mySocket = cxn->socket;
      cxn->env.serverSocket = socket;
      debug(DEBUG_INFO, "Connected to %s\n",
            inet_ntop(AF_INET6, &me.sin6_addr, v6buf, INET6_ADDRSTRLEN));

      fdarr_cntl(FDARR_ADD, cxn->socket, POLLIN);
      err = FDARR_MODIFIED;
      connections->insert(std::make_pair(cxn->socket, cxn));
   } else {
      debug(DEBUG_INFO, "mysock, revent: unknown, %08X, state: N/A\n", revents);
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
   struct buf* buf;
   enum CXN_STATE ps, ns;
   int res;
   char* ptr;

   ps = cxn->state;
   ns = ps;
   
   if (revents & POLLIN && ps == ST_REQUEST) {
      debug(DEBUG_INFO, "cxsock, revent: POLLIN, state: ST_REQUEST\n");
      return frbActions[fill_request_buffer(cxn->socket,
                                            cxn->request.buffer,
                                            &cxn->request.bytesUsed)](cxn);
   } else if (revents & POLLOUT && ps == ST_INTERNAL) {
      debug(DEBUG_INFO, "cxsock, revent: POLLOUT, state: ST_INTERNAL\n");

      buf = &cxn->response.buffer;
      ptr = buf->data + (buf->bytesUsed - buf->bytesToWrite);
      // In this state the request buffer (with HTTP headers) has been
      // prepared by error_response, or cgi_response, or json_response.
      // We send it, and we're done. The exception is for a normal CGI
      // response where we then have to fork.
      if ((res = write(cxn->socket, ptr, buf->bytesToWrite)) == -1) {
         // If there is an error from write, close the connection.
         // We can't even send a `500 Internal Error` since we can't
         // write on the socket.
         debug(DEBUG_WARNING, "write: %s\n", strerror(errno));
         return finish_response(cxn, false);
      } else if (buf->bytesToWrite - res == 0) {
         if (!(cxn->opts & CXN_CGI)) {
            return finish_response(cxn, false); // Fix this
         } else {
            ns = ST_CGI_FIL;
         }
      } else {
         buf->bytesToWrite -= res;
         ns = ps;
      }
   } else if (revents & POLLOUT && ps == ST_RESPONSE_WRIT) {
      debug(DEBUG_INFO, "cxsock, revent: POLLOUT, state: ST_RESPONSE_WRIT\n");

      buf = &cxn->response.buffer;
      ptr = buf->data + (buf->bytesUsed - buf->bytesToWrite);
      if ((res = write(cxn->socket, ptr, buf->bytesToWrite)) == -1) {
         debug(DEBUG_WARNING, "write: %s\n", strerror(errno));
         return finish_response(cxn, false);
      } else if (buf->bytesToWrite - res == 0 && cxn->response.eofFound) {
         return finish_response(cxn, false);
      } else if (buf->bytesToWrite - res == 0) {
         fdarr_cntl(FDARR_MODIFY, cxn->socket, 0);
         fdarr_cntl(FDARR_MODIFY, cxn->file, POLLIN);
         ns = ST_RESPONSE_READ;
      } else {
         buf->bytesToWrite -= res;
         cxn->state = ps;
      }
   } else if (revents & POLLOUT && ps == ST_CGI_FIL) {
      debug(DEBUG_INFO, "cxsock, revent: POLLOUT, state: ST_CGI_FIL\n");

      if (do_cgi(cxn) == POSIX_ERROR) {
         // If there is an error, send a 500 and "go back" in the
         // state machine to ST_INTERNAL. We do this by calling
         // this function again. Since we changed the state
         // the error actions will occur. We mannually specify the revent
         // that will cause the internal response code to run.
         error_response(&cxn->response, RESPONSE_INTERNAL_ERROR);
         cxn->opts &= ~CXN_CGI;
         cxn->state = ST_INTERNAL;
         return process_cxsock_events(cxn, POLLOUT);
      } else {
         return finish_response(cxn, false);
      }
   } else if (revents & POLLHUP || revents & POLLERR || revents & POLLNVAL) {
      debug(DEBUG_INFO, "cxsock, revent: POLLHUP/POLLERR/POLLNVAL\n");
      return finish_response(cxn, false);
   } else {
      debug(DEBUG_INFO, "cxsock, revent: unknown, %08X, state: N/A\n", revents);
   }

   cxn->state = ns;
   return 0;
}

static int process_cxfile_events(struct connection* cxn, short revents) {
   struct buf* buf;
   int res, readAmt, err = 0;

   if (revents & POLLIN && cxn->state == ST_RESPONSE_READ) {
      debug(DEBUG_INFO, "cxfile, revent: POLLIN, state: ST_RESPONSE_READ\n");
      
      buf = &cxn->response.buffer;
      // Find a better way of doing this later
      readAmt = cxn->opts & CXN_FORTUNE ? 0.9 * BUF_LEN : BUF_LEN;
      if ((res = read(cxn->file, buf->data, readAmt)) == -1) {
         debug(DEBUG_WARNING, "read: %s\n", strerror(errno));
         error_response(&cxn->response, RESPONSE_INTERNAL_ERROR);
         cxn->state = ST_INTERNAL;
         return 0;
      }

      cxn->response.eofFound = res == 0 || revents & POLLHUP;
      fdarr_cntl(FDARR_MODIFY, cxn->socket, POLLOUT);
      buf->bytesUsed = buf->bytesToWrite = res;
      cxn->state = ST_RESPONSE_WRIT;

      if (cxn->opts & CXN_FORTUNE && fortune_decode(buf) == POSIX_ERROR) {
         error_response(&cxn->response, RESPONSE_INTERNAL_ERROR);
         cxn->state = ST_INTERNAL;
      }
   } else {
      debug(DEBUG_WARNING, "cxfile, revent: unknown %08X, state: N/A\n",
            revents);
   }
   
   return err;
}

static int request_state_zero_read(struct connection* cxn) {
   debug(DEBUG_INFO, "Zero read, closing connection\n");
   finish_response(cxn, false);
   return FDARR_MODIFIED;
}

static int request_state_error(struct connection* cxn) {
   cxn->state = ST_INTERNAL;
   fdarr_cntl(FDARR_MODIFY, cxn->socket, POLLOUT);
   error_response(&cxn->response, RESPONSE_INTERNAL_ERROR);
   return 0;
}

static int request_state_incomplete(struct connection* cxn) {
   // Do nothing!
   return 0;
}

static int request_state_finished(struct connection* cxn) {
   char* ptr;

   fdarr_cntl(FDARR_MODIFY, cxn->socket, POLLOUT);
   switch(process_request(cxn)) {
   case HTTP_FORTUNE:
      cxn->opts |= CXN_FORTUNE;
      if (do_fortune(cxn) == POSIX_ERROR) {
         error_response(&cxn->response, RESPONSE_INTERNAL_ERROR);
         cxn->state = ST_INTERNAL;
         return 0;
      }

      cxn->response.contentType = "application/json";
      // Set the contentLength to -1 to indicate we're not
      // providing this field for this response.
      cxn->response.contentLength = -1;
      make_response_header(&cxn->response);
      //cxn->state = ST_RESPONSE_HEAD;
      cxn->state = ST_RESPONSE_WRIT;
      return FDARR_MODIFIED;
   case HTTP_CGI:
      cxn->opts |= CXN_CGI;
      cxn->state = ST_INTERNAL;
      return 0;
   case INTERNAL_RESPONSE:
      cxn->state = ST_INTERNAL;
      return 0;
   }

   //cxn->state = ST_RESPONSE_HEAD;
   cxn->state = ST_RESPONSE_WRIT;
   fdarr_cntl(FDARR_ADD, cxn->file, POLLIN);
   connections->insert(std::make_pair(cxn->file, cxn));
   ptr = strrchr(cxn->request.filepath, '.');
   if (ptr && ext2mime->count(std::string(ptr + 1))) {
      cxn->response.contentType =
         ext2mime->at(std::string(ptr +1)).c_str();
   }

   make_response_header(&cxn->response);
   return FDARR_MODIFIED;
}

static int finish_response(struct connection* cxn, bool keepAlive) {
   int socket;
   struct env env;

   log_access(cxn);

   if (connections->erase(cxn->file)) {
      fdarr_cntl(FDARR_REMOVE, cxn->file);
      close(cxn->file);
   }
   
   if (keepAlive) {
      socket = cxn->socket;
      env = cxn->env;
      fdarr_cntl(FDARR_MODIFY, cxn->socket, POLLIN);  
      memset(cxn, 0, sizeof(struct connection));
      cxn->socket = socket;
      cxn->env = env;
   } else {
      fdarr_cntl(FDARR_REMOVE, cxn->socket);
      close(cxn->socket);
      connections->erase(cxn->socket);
      free(cxn);
   }

   return FDARR_MODIFIED;
}

static int cgi_request(struct connection* cxn) {
   struct response* response = &cxn->response;
   enum CGI_CMD cmd = CGI_STATUS; // Initialize to default.
   enum RESPONSE_TYPE type;

   debug(DEBUG_INFO, "cgi request: %s\n", cxn->env.query);

   if ((type = parse_cgi_request(&cxn->env, &cmd)) != RESPONSE_OK) {
      return error_response(response, type);
   }

   debug(DEBUG_INFO, "Doing CGI action.\n");
   return cgi_response(&cxn->response, cmd);
}

static int json_request(struct connection* cxn) {
   enum JSON_CMD cmd;
   char* str = cxn->env.query + strlen("/json/");

   debug(DEBUG_INFO, "json request: >%s<\n", cxn->env.query);

   if (strcmp(cxn->env.query, "/json/") == 0) {
      return error_response(&cxn->response, RESPONSE_NOT_FOUND);
   } else if (strcmp(str, "about.json") == 0) {
      cmd = JSON_ABOUT;
   } else if (strcmp(str, "implemented.json") == 0) {
      cmd = JSON_IMPLEMENTED;
   } else if (strcmp(str, "fortune") == 0) {
      cmd = JSON_FORTUNE;
   } else {
      debug(DEBUG_WARNING, "Unsupported JSON operation requested: %s\n", str);
      return error_response(&cxn->response, RESPONSE_NOT_FOUND);
   }

   debug(DEBUG_INFO, "Doing some JSON action.\n");
   return json_response(&cxn->response, cmd);
}

static int file_request(struct connection* cxn) {
   struct stat stats;
   struct request* request = &cxn->request;
   struct response* response = &cxn->response;
   int fd;

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
   if ((fd = open(request->filepath, O_RDONLY | O_NONBLOCK)) == -1) {
      switch (errno) {
      case EACCES: return error_response(response, RESPONSE_FORBIDDEN);
      default:
         debug(DEBUG_WARNING, "popen: %s\n", strerror(errno));
         return error_response(response, RESPONSE_INTERNAL_ERROR);
      }
   }

   cxn->file = fd;
   return 0;
}

static int process_request(struct connection* cxn) {
   char* tmp = cxn->request.filepath + strlen("docs");
   enum RESPONSE_TYPE type;

   type = make_request_header(&cxn->request);
   print_request(&cxn->request);

   // We're not supporing deflate right now.
   cxn->response.usingDeflate = false;
   cxn->response.noKeepAlive = cxn->request.noKeepAlive;
   cxn->env.method = requestType2String[cxn->request.type];
   cxn->env.query = tmp;

   if (type != RESPONSE_OK) {
      return error_response(&cxn->response, type);
   }

   if ((strstr(tmp, "/cgi-bin/"))) {
      return cgi_request(cxn);
   } else if (strstr(tmp, "/json/")) {
      return json_request(cxn);
   } else {
      return file_request(cxn);
   }
}

static enum REQUEST_STATE fill_request_buffer(int socket,
                                              char* buffer,
                                              unsigned int* bytesUsed) {
   int bytesRead;

   if ((bytesRead = read(socket,
                         buffer + *bytesUsed,
                         BUF_LEN - *bytesUsed)) == -1) {
      debug(DEBUG_WARNING, "read: %s\n", strerror(errno));
      return ST_ERROR;
   } else if (bytesRead == 0) {
      return ST_ZERO_READ;
   }

   *bytesUsed += bytesRead;

   if (*bytesUsed >= BUF_LEN) {
      debug(DEBUG_WARNING, "Request buffer full.\n");
      return ST_ERROR;
   }

   if (memmem(buffer, *bytesUsed, "\r\n\r\n", strlen("\r\n\r\n")) == NULL) {
      return ST_INCOMPLETE;
   } else {
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
            "%d to %d bytes\n", nfds, fdarrMaxSize);
      fds = (struct pollfd *) realloc(fds,
                                      fdarrMaxSize * sizeof(struct pollfd));
      if (fds == NULL) {
         debug(DEBUG_ERROR, "Allocating pollfd array failed\n");
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

      if (i != nfds - 1 && nfds > 1) {
         memcpy(fds + i, fds + nfds - 1, sizeof(struct pollfd));
      }
      nfds--;
      break;
   case FDARR_CLEAN_UP:
      free(fds);
      break;
   }

   va_end(args);
   return err;
}

static int json_response(struct response* response, enum JSON_CMD cmd) {
   int bytesWritten = 0;
   char buf[BUF_LEN];

   response->type = RESPONSE_OK;
   response->contentType = "application/json";

   switch(cmd) {
   case JSON_FORTUNE:
      debug(DEBUG_INFO, "JSON: fortune\n");
      return HTTP_FORTUNE;
   case JSON_IMPLEMENTED:
      debug(DEBUG_INFO, "JSON: implemented.json\n");
      bytesWritten = sprintf(buf, "%s", BODY_JSON_IMPLEMENTED);
      break;
   case JSON_ABOUT:
      debug(DEBUG_INFO, "JSON: about.json\n");
      bytesWritten = sprintf(buf, "%s", BODY_JSON_ABOUT);
      break;
   }

   response->contentLength = bytesWritten;
   make_response_header(response);
   memcpy(response->buffer.data + response->headerLength, buf, bytesWritten);
   response->buffer.bytesUsed += bytesWritten;
   response->buffer.bytesToWrite = response->buffer.bytesUsed;
   return INTERNAL_RESPONSE;
}

//an error response is a response that's not the normal HTTP 200 OK
static int error_response(struct response* response, enum RESPONSE_TYPE type) {
   static const char* responseType2Desc[NUM_HTTP_RESPONSE_TYPES] = {
      "",
      "The request could not be understood by this server.",
      "The method is not allowed for the requested url.",
      "",
      "Page not found.",
      "The request could not be completed due to "
      "encountering an internal error"
   };
   char buf[BUF_LEN];
   int bytesWritten = 0;
   const char* title = responseType2String[type];

   debug(DEBUG_INFO, "Sending %s\n", title);
   bytesWritten = snprintf(buf, BUF_LEN, ERROR_BODY, title, title,
                           responseType2Desc[type]);
   response->contentLength = bytesWritten;
   response->contentType = "text/html";
   response->type = type;
   make_response_header(response);
   memcpy(response->buffer.data + response->headerLength, buf, bytesWritten);
   response->buffer.bytesUsed += bytesWritten;
   response->buffer.bytesToWrite = response->buffer.bytesUsed;
   return INTERNAL_RESPONSE;
}

static int cgi_response(struct response* response, enum CGI_CMD cmd) {
   char buf[BUF_LEN];
   time_t t = time(NULL);
   int bytesWritten = 0;

   if (cmd == CGI_DO) {
      debug(DEBUG_INFO, "Sending CGI response line\n");
      bytesWritten = snprintf(response->buffer.data, BUF_LEN, "HTTP/1.1 200 OK\r\n");
      response->buffer.bytesToWrite = response->buffer.bytesUsed =
         response->headerLength = bytesWritten;
      return HTTP_CGI;
   }

   debug(DEBUG_INFO, "Sending CGI status message\n");
   bytesWritten = sprintf(
      buf, "%sServer Process ID: %d<BR>\nCurrent Time: %s%s",
      BODY_STATUS_BEGIN, getpid(), asctime(localtime(&t)), BODY_STATUS_END);

   response->contentLength = bytesWritten;
   response->contentType = "text/html";
   response->type = RESPONSE_OK;
   make_response_header(response);
   memcpy(response->buffer.data + response->headerLength, buf, bytesWritten);
   response->buffer.bytesUsed += bytesWritten;
   response->buffer.bytesToWrite = response->buffer.bytesUsed;
   return INTERNAL_RESPONSE;
}

static void make_response_header(struct response* response) {
   struct buf* buf = &response->buffer;
   int written;

   written = snprintf(buf->data, BUF_LEN, "HTTP/1.1 %s\r\n",
                      responseType2String[response->type]);

   if (response->contentType) {
      written += snprintf(buf->data + written, BUF_LEN - written,
                          "Content-Type: %s\r\n", response->contentType);
   }

   if (response->noKeepAlive) {
      written += snprintf(buf->data + written, BUF_LEN - written,
                          "Connection: Close\r\n");
   }

   if (response->contentLength >= 0) {
      written += snprintf(buf->data + written, BUF_LEN - written,
                          "Content-Length: %d\r\n", response->contentLength);
   }

   written += snprintf(buf->data + written, BUF_LEN - written, "\r\n");
   buf->bytesUsed = buf->bytesToWrite = response->headerLength = written;
}

static int generate_listing(char* filepath, struct response* response) {
   char* buf;
   int bytesWritten = 0, numDirEntries = 0;
   DIR* parent;
   char* str = (char *) calloc(1, strlen(filepath) - strlen("docs") + 1);
   struct dirent* entry;

   debug(DEBUG_INFO, "In generate_listing\n");

   if (str == NULL) {
      debug(DEBUG_WARNING, "Allocating filepath copy failed\n");
      return error_response(response, RESPONSE_INTERNAL_ERROR);
   }

   memcpy(str, filepath + strlen("docs"),
          strlen(filepath) - strlen("docs") + 1);
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
   memcpy(response->buffer.data + response->headerLength, buf, bytesWritten);
   response->buffer.bytesUsed += bytesWritten;
   response->buffer.bytesToWrite = response->buffer.bytesUsed;

   free(str);
   free(buf);
   return INTERNAL_RESPONSE;
}

//parse the request declaration
//will modify the string in the request buffer via strtok!
static enum RESPONSE_TYPE parse_request_declaration(struct request* request,
                                                    char** filepath) {
   int i;
   char* tok;
   char* toks[NUM_REQ_DECL_PARTS];

   //separate out the request declaration into tokens
   for (i = 0, tok = strtok(request->buffer, " \n");
        tok;
        i++, tok = strtok(NULL, " \n")) {
      if (i > NUM_REQ_DECL_PARTS - 1) {
         debug(DEBUG_INFO, "Invalid HTTP request declaration\n");
         return RESPONSE_BAD_REQUEST;
      }
      toks[i] = tok;
   }

   if (strcasestr(toks[0], "GET")) {
      request->type = REQUEST_GET;
   } else {
      debug(DEBUG_INFO, "Method not supported: %s\n", toks[0]);
      return RESPONSE_METHOD_NOT_ALLOWED;
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
      debug(DEBUG_INFO, "Unsupported HTTP protocol version (!).\n");
      return RESPONSE_BAD_REQUEST;
   }

   return RESPONSE_OK;
}

//the return value of the function indicates whether there were any errors
//all modification is done through pointers
static enum RESPONSE_TYPE make_request_header(struct request* request) {
   char* lineEnd, * tok, * tmp;
   char* lines[MAX_TOKS];
   int i, numToks;
   enum RESPONSE_TYPE res;

   //find the end of the first line of the request
   if ((lineEnd = (char *) memmem(request->buffer, request->bytesUsed,
                                   "\r\n", 2)) == NULL) {
      debug(DEBUG_INFO, "Can't find HTTP request declaration\n");
      return RESPONSE_BAD_REQUEST;
   }
   *lineEnd = *(lineEnd + 1) = '\0';

   if ((res = parse_request_declaration(request, &tmp)) != RESPONSE_OK) {
      return res;
   }

   //split the remainder of the request up
   for (i = 0, tok = strtok(lineEnd + 2, "\r\n");
        tok;
        i++, tok = strtok(NULL, "\r\n")) {
      if (i >= MAX_TOKS) {
         debug(DEBUG_WARNING, "Overflowed header lines buffer!\n");
         return RESPONSE_INTERNAL_ERROR;
      }
      lines[i] = tok;
   }
   numToks = i;

   request->referer = "";
   request->userAgent = "";

   for (i = 0; i < numToks; i++) {
      if (strcasecmp(lines[i], "Connection: Close") == 0) {
         request->noKeepAlive = true;
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

   if (url_decode(tmp) == POSIX_ERROR) {
      return RESPONSE_INTERNAL_ERROR;
   }

   snprintf(request->filepath, NAME_BUF_LEN, "docs%s", tmp);
   return RESPONSE_OK;
}

static inline const char* bool_to_string(bool b) {
   return b ? "true" : "false";
}

static char* request_declaration_to_string(const struct request* request) {
   static char str[NAME_BUF_LEN];

   snprintf(str, NAME_BUF_LEN, "%s %s HTTP/%.1lf",
            requestType2String[request->type],
            request->filepath + strlen("docs"),
            request->httpVersion / 10.0 + 1.0);
   return str;
}

//make this into a to_string method some time in the future
static void print_request(struct request* request) {
   debug(DEBUG_INFO, "Request: %s\n", request_declaration_to_string(request));
   debug(DEBUG_INFO, "|-HTTP-Option Referrer: %s\n", request->referer);
   debug(DEBUG_INFO, "|-HTTP-Option User-Agent: %s\n", request->userAgent);
   debug(DEBUG_INFO, "|-HTTP-Option `Connection: Close`?: %s\n",
         bool_to_string(request->noKeepAlive));
   debug(DEBUG_INFO, "|-HTTP-Option `Accept-Encoding: deflate`?: %s\n",
         bool_to_string(request->acceptDeflate));
}

// Quick and dirty JSON string escaping function. Improve Later.
// As written may could easily create a buffer overflow error
static int fortune_decode(struct buf* buf) {
   // extra counts the number of extra bytes we have from
   // escaping the string characters
   int extra = 0;
   char* tmp = (char *) malloc(buf->bytesUsed);
   char* tmpPtr = tmp, * bufPtr = buf->data;

   if (tmp == NULL) {
      return POSIX_ERROR;
   }

   memcpy(tmp, buf->data, buf->bytesUsed);
   while (tmpPtr - tmp < buf->bytesUsed && extra < BUF_LEN - buf->bytesUsed) {
      if (*tmpPtr == '"') {
         extra++;
         *bufPtr++ = '\\';
      }
      *bufPtr++ = *tmpPtr++;
   }
   buf->bytesUsed += extra;
   buf->bytesToWrite = buf->bytesUsed;
   free(tmp);

   if (buf->bytesUsed >= BUF_LEN) {
      debug(DEBUG_WARNING, "Buffer overflow in fortune_decode\n");
      return POSIX_ERROR;
   }
   return 0;
}

// There will be no buffer overflow errors in this function because decoding
// the hex codes for the invalid URL chars, e.g. `%20` -> ' ' always results
// in fewer characters.
static int url_decode(char* url) {
   int len = strlen(url);
   char* tmp = (char *) malloc(len + 1);
   char hexBuf[3];
   int hexValue;
   char* urlPtr = url, * tmpPtr = tmp;

   if (tmp == NULL) {
      return POSIX_ERROR;
   }

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
   return 0;
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
static enum RESPONSE_TYPE parse_cgi_request(struct env* env,
                                            enum CGI_CMD* cmd) {
   socklen_t sockSize = sizeof(struct sockaddr_in6);
   struct sockaddr_in6 me;
   struct sockaddr_in6 server;
   char* pathEnd, * envvarSpace;
   char v6buf[INET6_ADDRSTRLEN];
   int len, i;
   bool hasParams = false;

   if (strcmp(env->query + strlen("/cgi-bin/"), "status") == 0) {
      *cmd = CGI_STATUS;
      return RESPONSE_OK;
   }

   if (strcmp(env->query, "/cgi-bin/") == 0) {
      return RESPONSE_NOT_FOUND;
   }

   len = strlen(env->query) - strlen("/cgi-bin/");
   if ((pathEnd = strchr(env->query, '?'))) {
      len -= (len - (pathEnd - (env->query + strlen("/cgi-bin/"))));
      hasParams = true;
   }

   env->filepath = (char *) calloc(1, len + strlen("cgi/") + 1);
   if (env->filepath == NULL) {
      debug(DEBUG_WARNING, "Failed to allocate cgi filepath\n");
      return RESPONSE_INTERNAL_ERROR;
   }

   memcpy(env->filepath + strlen("cgi/"), env->query + strlen("/cgi-bin/"),
          len);
   (env->filepath + strlen("cgi/"))[len] = '\0';
   memcpy(env->filepath, "cgi/", strlen("cgi/"));
   debug(DEBUG_INFO, "cgi filepath: %s\n", env->filepath);

   if (access(env->filepath, X_OK)) {
      free(env->filepath);
      switch(errno) {
      case ENOENT: return RESPONSE_NOT_FOUND;
      case EACCES: return RESPONSE_FORBIDDEN;
      default: return RESPONSE_INTERNAL_ERROR;
      }
   }

   if (getsockname(env->serverSocket, (struct sockaddr *) &server, &sockSize) ||
      getsockname(env->mySocket, (struct sockaddr *) &me, &sockSize)) {
      debug(DEBUG_WARNING, "Error getting server socket name\n");
      free(env->filepath);
      return RESPONSE_INTERNAL_ERROR;
   }

   envvarSpace = (char *) calloc(NUM_ENV_VARS, ENV_BUF_LEN);
   if (envvarSpace == NULL) {
      debug(DEBUG_WARNING, "Allocating envvar space failed\n");
      free(env->filepath);
      return RESPONSE_INTERNAL_ERROR;
   }

   for (i = 0; i < NUM_ENV_VARS; i++) {
      env->envvars[i] = envvarSpace + i * ENV_BUF_LEN;
   }

   strcpy(env->envvars[0], "GATEWAY_INTERFACE=CGI/1.1");
   snprintf(env->envvars[1], ENV_BUF_LEN, "QUERY_STRING=%s",
            hasParams ? env->query + strlen("/cgi-bin/") + len + 1 : "");

   snprintf(env->envvars[2], ENV_BUF_LEN, "REMOTE_ADDR=[%s]",
            inet_ntop(AF_INET6, &me.sin6_addr, v6buf, INET6_ADDRSTRLEN));

   snprintf(env->envvars[3], ENV_BUF_LEN, "REQUEST_METHOD=%s",
            env->method);

   snprintf(env->envvars[4], ENV_BUF_LEN, "SCRIPT_NAME=/cgi-bin/%s",
            env->filepath + strlen("cgi/"));

   snprintf(env->envvars[5], ENV_BUF_LEN, "SERVER_NAME=[%s]",
            inet_ntop(AF_INET6, &server.sin6_addr, v6buf, INET6_ADDRSTRLEN));

   snprintf(env->envvars[6], ENV_BUF_LEN, "SERVER_PORT=%d",
            ntohs(server.sin6_port));

   strcpy(env->envvars[7], "SERVER_PROTOCOL=HTTP/1.1");
   env->envvars[8] = NULL;

   *cmd = CGI_DO;
   return RESPONSE_OK;
}

static int do_fortune(struct connection* cxn) {
   int fortunePipe[2];

   debug(DEBUG_INFO, "Fortune operation in progress\n");
   if (pipe(fortunePipe) == -1) {
      debug(DEBUG_WARNING, "pipe: %s\n", strerror(errno));
      return POSIX_ERROR;
   }

   if ((cxn->env.childPid = fork()) == -1) {
      debug(DEBUG_WARNING, "fork: %s\n", strerror(errno));
      return POSIX_ERROR;
   } else if (cxn->env.childPid == 0) { //the child
      // Close the read pipe
      close(fortunePipe[0]);
      //dup the write pipe
      dup2(fortunePipe[1], STDOUT_FILENO);
      execlp("fortune", "fortune", NULL);
      debug(DEBUG_ERROR, "exec: %s\n", strerror(errno));
      exit(EXIT_FAILURE);
   } else { // the parent
      cxn->file = fortunePipe[0];

      // Close the write pipe
      close(fortunePipe[1]);
      // Make the read pipe non blocking.
      fcntl(cxn->file, F_SETFL, fcntl(cxn->file, F_GETFL) & ~O_NONBLOCK);
      // Add the read pipe to the fdarr.
      fdarr_cntl(FDARR_ADD, cxn->file, POLLIN);
      connections->insert(std::make_pair(cxn->file, cxn));
   }
   return 0;
}

static int do_cgi(struct connection* cxn) {
   struct env* env = &cxn->env;
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
      debug(DEBUG_WARNING, "exec: %s\n", strerror(errno));
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
   struct connection* cxn;

   fdarr_cntl(FDARR_CLEAN_UP);
   fclose(accessLog);

   for (itr = connections->begin(); itr != connections->end(); itr++) {
      cxn = itr->second;
      if (cxn) {
         if (connections->count(cxn->file)) {
            connections->at(cxn->file) = NULL;
            close(cxn->file);
         }
         close(cxn->socket);
         free(cxn);
      }
   }
   delete connections;
   delete ext2mime;
   close(mySock);
   debug(DEBUG_INFO, "Exiting cleanly.\n");
   exit(EXIT_SUCCESS);
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
   debug(DEBUG_ERROR, "%s: %s\n", str, strerror(errno));
   exit(EXIT_FAILURE);
}
