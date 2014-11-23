#!/bin/sh

echo "Content-Type: text/html"
echo
echo "<HTML><HEAD><TITLE>CGI Script Parameter Table</TITLE></HEAD><BODY>"
echo "<TABLE BORDER=\"1\"><TR><TH>Parameter Name</TH><TH>Value</TH></TR>"
echo "<TR><TD>GATEWAY_INTERFACE</TD><TD>$GATEWAY_INTERFACE</TD></TR>"
echo "<TR><TD>QUERY_STRING</TD><TD>$QUERY_STRING</TD></TR>"
echo "<TR><TD>REMOTE_ADDR</TD><TD>$REMOTE_ADDR</TD></TR>"
echo "<TR><TD>REQUEST_METHOD</TD><TD>$REQUEST_METHOD</TD></TR>"
echo "<TR><TD>SCRIPT_NAME</TD><TD>$SCRIPT_NAME</TD></TR>"
echo "<TR><TD>SERVER_NAME</TD><TD>$SERVER_NAME</TD></TR>"
echo "<TR><TD>SERVER_PORT</TD><TD>$SERVER_PORT</TD></TR>"
echo "<TR><TD>SERVER_PROTOCOL</TD><TD>$SERVER_PROTOCOL</TD></TR>"
echo "</TABLE></BODY></HTML>"
