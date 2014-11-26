#!/bin/sh

echo -n -e "Content-Type: text/html\r\n"
echo -n -e "Foo-Header: bar\r\n"
echo -n -e "\r\n"
echo -n -e "<HTML><HEAD><TITLE>Test CGI Script</TITLE></HEAD>\r\n"
echo -n -e "<BODY>Simple CGI Test!</BODY></HTML>\r\n"
