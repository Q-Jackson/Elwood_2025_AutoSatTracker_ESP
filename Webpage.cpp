/* this class handles the main web page and all interactions
 *
 * N.B. do not edit the html, edit ast.html then use preppage.pl. See README.
 */

#include <ctype.h>

#include "Webpage.h"


/* constructor
 */
Webpage::Webpage()
{
	// ask user how to connect to wifi if we can not attach
	while (!connectWiFi())
	    askWiFi();

	// create server
	resetWatchdog();
	Serial.println ("Creating ethernet server");
	httpServer = new WiFiServer(80);				// http
	httpServer->begin();

	// init user message mechanism
	user_message_F = F("Hello+");					// page welcome message
	memset (user_message_s, 0, sizeof(user_message_s));

	// init TLE storage and state
	memset (&tlef, 0, sizeof(tlef));
	tlef.running = false;
}

/* try to connect to wifi using creds we have in EEPROM
 * return whether it connected ok
 */
bool Webpage::connectWiFi()
{
        // good dns
        IPAddress dns1(8,8,8,8);
        IPAddress dns2(208,67,222,222);

	// start over
	WiFi.disconnect(true);
	WiFi.softAPdisconnect(true);
	delay(400);

        // configure
	WiFi.mode(WIFI_STA);
	WiFi.config (nv->IP, nv->GW, nv->NM, dns1, dns2);

        // start
	WiFi.begin (nv->ssid, nv->pw);

	// wait for connect
	uint32_t t0 = millis();
	uint32_t timeout = 15000;					// timeout, millis()
	while (WiFi.status() != WL_CONNECTED) {
	    resetWatchdog();
	    if (millis() - t0 > timeout) {
		Serial.println (F("connect failed, starting as AP"));
		return (false);
	    }
	    delay(100);
	}

	Serial.println (WiFi.localIP());
	Serial.println (WiFi.gatewayIP());
	Serial.println (WiFi.dnsIP());

	// ok
	return (true);
}

/* remove non-alphanumeric chars and convert to upper case IN PLACE
 */
void Webpage::scrub (char *s)
{
	char *scrub_s;
	for (scrub_s = s; *s != '\0'; s++)
	    if (isalnum(*s))
		*scrub_s++ = toupper(*s);
	*scrub_s = '\0';
}

/* call this occasionally to check for Ethernet activity
 */
void Webpage::checkEthernet()
{
	resetWatchdog();

	// do more of remote fetch if active
	resumeTLEFetch();

	// now check our page
	WiFiClient client = httpServer->available();
	if (!client)
	    return;

	// Serial.println ("client connected");
	uint32_t to = millis();		// init timeout
	char firstline[128];		// first line
	unsigned fll = 0;		// firstline length
	bool eoh = false;		// set when see end of header
	bool fldone = false;		// set when finished collecting first line
	char c, prevc = 0;		// new and previous character read from client

	// read header, collecting first line and discarding rest until find blank line
	while (!eoh && (c = readNextClientChar (client, &to))) {
	    if (c == '\n') {
		if (!fldone) {
		    firstline[fll] = '\0';
		    fldone = true;
		}
		if (prevc == '\n')
		    eoh = true;
	    } else if (!fldone && fll < sizeof(firstline)-1) {
		firstline[fll++] = c;
	    }
	    prevc = c;
	}
	if (c == 0) {
	    // Serial.println ("closing client");
	    client.stop();
	    return;
	}

	// client socket is now at first character after blank line

	// replace trailing ?time cache-buster with blank
	char *q = strrchr (firstline, '?');
	if (q)
	    *q = ' ';

	// what we do next depends on first line
	resetWatchdog();
	// Serial.println (firstline);
	if (strstr (firstline, "GET / ")) {
	    sendMainPage (client);
	} else if (strstr (firstline, "GET /getvalues.txt ")) {
	    sendNewValues (client);
	} else if (strstr (firstline, "POST / ")) {
	    overrideValue (client);
	    sendEmptyResponse (client);
	} else if (strstr (firstline, "POST /reboot ")) {
	    sendEmptyResponse (client);
	    reboot();
	} else if (strstr (firstline, "POST /start ")) {
	    target->setTrackingState(true);
	    sendEmptyResponse (client);
	} else if (strstr (firstline, "POST /stop ")) {
	    target->setTrackingState(false);
	    sendEmptyResponse (client);
	} else {
	    send404Page (client);
	}

	// finished
	// Serial.println ("closing client");
	client.stop();
}

/* given "sat,URL" search the given URL for the given satellite TLE.
 * N.B. in order for our web page to continue to function, this method is just the first step, other
 *   steps are done incrementally by resumeTLEFetch().
 */
void Webpage::startTLEFetch (char *query_text)
{
	// split query at , to get sat name and URL
	char *sat = query_text;
	char *url = strchr (query_text, ',');
	if (!url) {
	    setUserMessage (F("Invalid querySite string: "), query_text, '!');
	    return;
	}
	*url++ = '\0';		// overwrite , with EOS for sat then move to start of url

	// remove leading protocol, if any
	if (strncmp (url, "http://", 7) == 0)
	    url += 7;

	// file name
	char *path = strchr (url, '/');
	if (!path) {
	    setUserMessage (F("Invalid querySite URL: "), url, '!');
	    return;
	}
	*path++ = '\0';		// overwrite / with EOS for server then move to start of path

	// connect
	tlef.remote = new WiFiClient();
	if (!tlef.remote->connect (url, 80)) {
            Serial.print (F("connect failed\n"));
	    setUserMessage (F("Failed to connect to "), url, '!');
	    delete tlef.remote;
	    return;
	}

	// send query to retrieve the file containing TLEs
	// Serial.print(sat); Serial.print(F("@")); Serial.println (url);
	tlef.remote->print (F("GET /"));
	tlef.remote->print (path);
	tlef.remote->print (F(" HTTP/1.1\r\n")); // 6/26/26 changed to http 1.1
	tlef.remote->print (F("Host: "));        // 6/26/26 added line
  tlef.remote->print (url);                // 6/26/26 added line
	tlef.remote->print (F("\r\n"));          // 6/26/26 added line
	tlef.remote->print (F("Content-Type: text/plain\r\n"));
	tlef.remote->print (F("User-Agent: Sat Tracker\r\n"));
	tlef.remote->print (F("\r\n"));

	// set up so we can resume the search later....
	scrub (sat);
	strncpy (tlef.sat, sat, sizeof(tlef.sat)-1);
	tlef.lineno = 1;
	tlef.running = true;
}

/* called to resume fetching a remote web page, started by startTLEFetch().
 * we are called periodically regardless, do nothing if no fetch is in progress.
 * we know to run based on whether tlef.remote is connected.
 */
void Webpage::resumeTLEFetch ()
{
	// skip if nothing in progress
	if (!tlef.running)
	    return;

	// init
	const uint32_t tout = millis() + 10000;		// timeout, ms
	uint8_t nfound = 0;				// n good lines found so far
	char *bp = tlef.buf;				// next buf position to use
	tlef.l0 = NULL;					// flag for sendNewValues();

	// read another line, if find sat read two more and finish up
	while (tlef.remote->connected() && nfound < 3 && millis() < tout) {
	    if (tlef.remote->available()) {
		char c = tlef.remote->read();
		if (c == '\r')
		    continue;
		if (c == '\n') {
		    // show some progress
		    char lnbuf[10];
		    setUserMessage (F("Reading line "), itoa(tlef.lineno++, lnbuf, 10), '+');

		    *bp++ = '\0';
		    switch (nfound) {
		    case 0:
			char sl0[50];			// copy enough that surely contains sat
			strncpy (sl0, tlef.buf, sizeof(sl0)-1);
			sl0[sizeof(sl0)-1] = '\0';	// insure EOS
			scrub (sl0);			// scrub the copy so l0 remains complete
			if (strstr (sl0, tlef.sat)) {
			    // found sat, prepare to collect TLE line 1 in l1
			    nfound++;			// found name
			    tlef.l0 = tlef.buf;		// l0 begins at buf
			    tlef.l1 = bp;		// l1 begins at next buf position
			} else
			    return;			// try next line on next call
			break;
		    case 1:
			if (target->tleValidChecksum(tlef.l1)) {
			    // found TLE line 1, prep for line 2
			    nfound++;			// found l1
			    tlef.l2 = bp;		// l2 begins at next buf position
			} else {
			    nfound = 0;			// no good afterall
			    tlef.l0 = NULL;		// reset flag for sendNewValues()
			}
			break;
		    case 2:
			if (target->tleValidChecksum(tlef.l2)) {
			    // found last line
			    nfound++;			// found l2
			} else {
			    nfound = 0;			// no good afterall
			    tlef.l0 = NULL;		// reset flag for sendNewValues()
			}
			break;
		    default:
			// can't happen ;-)
			break;
		    }
		} else if (bp - tlef.buf < (int)(sizeof(tlef.buf)-1))
		    *bp++ = c;				// add to buf iif room, including EOS
	    } else {
		// static long n;
		// Serial.println (n++);
	    }
	}

	// get here if remote disconnected, found sat or timed out

	if (!tlef.remote->connected())
	    setUserMessage (F("TLE not found!"));
	else if (nfound == 3)
	    setUserMessage (F("Found TLE: "), tlef.l0, '+');
	else
	    setUserMessage (F("Remote site timed out!"));

	// finished regardless
	tlef.remote->stop();
	delete tlef.remote;
	tlef.running = false;
}

/* record a brief F() message to inform the user, it will be sent on the next sendNewValues() sweep
 */
void Webpage::setUserMessage (const __FlashStringHelper *ifsh)
{
	user_message_F = ifsh;
	user_message_s[0] = '\0';
}

/* record a brief message to inform the user, it will be sent on the next sendNewValues() sweep.
 * the message consists of an F() string, then a stack string, then a trailing character, typically
 *  '!' to indicate an alarm, '+' to indicate good progress, or '\0' for no effect.
 */
void Webpage::setUserMessage (const __FlashStringHelper *ifsh, const char *msg, char state)
{
	user_message_F = ifsh;
	strncpy (user_message_s, msg, sizeof(user_message_s)-2);	// room for state and EOS
	user_message_s[strlen(user_message_s)] = state;
}

/* read next character, return 0 if it disconnects or times out.
 * '\r' is discarded completely.
 */
char Webpage::readNextClientChar (WiFiClient &client, uint32_t *to)
{
	static const int timeout = 1000;		// client socket timeout, ms
	resetWatchdog();
	while (client.connected()) {
	    if (millis() > *to + timeout) {
		Serial.println ("client timed out");
		return (0);
	    }
	    if (!client.available())
		continue;
	    char c = client.read();
	    *to = millis();
	    if (c == '\r')
		continue;
	    // Serial.write(c);
	    return (c);
	}
	// Serial.println ("client disconnected");
	return (0);
}

/* op has entered manually a value to be overridden.
 * client is at beginning of NAME=VALUE line, parse and send to each subsystem
 * N.B. a few are treated specially.
 */
void Webpage::overrideValue (WiFiClient &client)
{
	char c, buf[200];			// must be at least enough for a known-valid TLE
	uint8_t nbuf = 0;			// type must be large enough to count to sizeof(buf)

	// read next line into buf
	uint32_t to = millis();		// init timeout
	while ((c = readNextClientChar (client, &to)) != 0) {
	    if (c == '\n') {
		buf[nbuf++] = '\0';
		break;
	    } else if (nbuf < sizeof(buf)-1)
		buf[nbuf++] = c;
	}
	if (c == 0)
	    return;		// bogus; let caller close

	// break at = into name, value
	char *valu = strchr (buf, '=');
	if (!valu)
	    return;		// bogus; let caller close
	*valu++ = '\0';	// replace = with 0 then valu starts at next char
	// now buf is NAME and valu is VALUE

	Serial.print (F("Override: ")); Serial.print (buf); Serial.print("="); Serial.println (valu);

	if (strcmp (buf, "T_TLE") == 0) {

	    // T_TLE needs two more lines

	    char *l1 = valu;		// TLE target name is valu
	    char *l2 = &buf[nbuf];	// line 2 begins after valu
	    char *l3 = NULL;		// set when find end of line 2

	    // scan for two more lines
	    uint8_t nlines = 1;
	    while (nlines < 3 && (c = readNextClientChar (client, &to)) != 0) {
		if (c == '\n') {
		    buf[nbuf++] = '\0';
		    if (++nlines == 2)
			l3 = &buf[nbuf];	// line 3 starts next
		} else if (nbuf < sizeof(buf)-1)
		    buf[nbuf++] = c;
	    }
	    if (nlines < 3)
		return;	// premature end, let caller close

	    // new target!
	    target->setTLE (l1, l2, l3);

	} else if (strcmp (buf, "IP") == 0) {

	    // op is setting a new IP, save in EEPROM for use on next reboot
	    char *octet = valu;
	    for (uint8_t i = 0; i < 4; i++) {
		int o = atoi(octet);
		if (o < 0 || o > 255)
		    return;				// bogus IP
		nv->IP[i] = o;
		if (i == 3)
		    break;
		octet = strchr (octet, '.');		// find next .
		if (!octet)
		    return;				// bogus format
		octet++;				// point to first char after .
	    }
	    nv->put();
	    setUserMessage (F("Successfully stored new IP address in EEPROM -- reboot to engage+"));

	} else if (strcmp (buf, "querySite") == 0) {

	    // op wants to look up a target at a web site, valu is target,url
	    startTLEFetch (valu);

	} else {

	    // not ours, give to each other subsystem in turn until one accepts
	    if (!circum->overrideValue (buf, valu)
			&& !gimbal->overrideValue (buf, valu)
			&& !target->overrideValue (buf, valu)
			&& !sensor->overrideValue (buf, valu))
		setUserMessage (F("Bug: unknown override -- see Serial Monitor!"));

	}
}

/* inform each subsystem to send its latest values, including ours
 */
void Webpage::sendNewValues (WiFiClient &client)
{
	// send plain text header for NAME=VALUE pairs
	sendPlainHeader(client);

	// send user message
	client.print ("op_message=");
	if (user_message_F != NULL)
	    client.print (user_message_F);
	if (user_message_s[0])
	    client.print (user_message_s);
	client.println();

	// send our values
	client.print ("IP=");
	for (uint8_t i = 0; i < 4; i++) {
	    client.print (nv->IP[i]);
	    if (i < 3)
		client.print (F("."));
	}
	client.println (F(""));

	if (tlef.l0) {
	    // set newly fetched name on web page
	    client.print (F("new_TLE="));
	    client.println (tlef.l0);
	    client.println (tlef.l1);
	    client.println (tlef.l2);
	    tlef.l0 = NULL;			// just send once
	}

	client.print (F("uptime="));
	circum->printSexa (client, millis()/1000.0/3600.0);
	circum->printPL (client, Circum::NORMAL);

	// send whatever the other modules want to
	circum->sendNewValues (client);
	gimbal->sendNewValues (client);
	sensor->sendNewValues (client);
	target->sendNewValues (client);

}

static const char main_page[] PROGMEM = R"_raw_html_(
<!DOCTYPE html>
<html>
<head>
    <meta http-equiv='Content-Type' content='text/html; charset=UTF-8' />
    <title>Sat Tracker</title>

    <style>

	body {
	    background-color:#888;
	    font-family:sans-serif;
	    font-size:13px;
	}

	table {
	    border-collapse: collapse;
	    border: 3px solid brown;
	    background-color:#F8F8F8;
	    float:left;
	}

	th {
	    padding: 6px;
	    border: 1px solid brown;
	}

	.even-row {
	    background-color:#F8F8F8;
	}

	.odd-row {
	    background-color:#D8D8D8;
	}

	#title-row {
	    text-align: center;
	    padding: 2px;
	    border-bottom: 6px double brown;
	}

	#title-label {
	    font-size: 18px;
	    font-weight: bold;
	    color: #0066CC;
	}

	#title-attrib {
	    font-size: 8px;
	    font-weight: bold;
	    color: #0066CC;
	}

	a {
	    color: #0066CC;
	}

	#op_message {
	    font-size:16px;
	    display: block;
	    padding: 10px;
	}

	td {
	    padding: 6px;
	    border: 1px solid #0066CC;
	}

	.TLE-display {
	    background-color:#D8D8D8;
	    font-family:monospace;
	    resize:none;
	    font-size:inherit;
	    overflow:hidden;
	    border: 1px solid brown;
	}

	.TLE-entry {
	    background-color:#FFF;
	    font-family:monospace;
	    resize:none;
	    font-size:inherit;
	    overflow:hidden;
	    border: 1px solid brown;
	}

	.major-section {
	    border-top: 6px double brown;
	}

	.minor-section {
	    border-top: 4px double brown;
	}

	.override {
	    background-color:#FFF;
	    padding: 0px;
	    font-family:monospace;
	    resize:none;
	    font-size:inherit;
	    width:7em;
	}

	.group-head {
	    text-align:center;
	    vertical-align:top;
	    border-right: 4px double brown;
	}

	.datum-label {
	    text-align:left;
	    vertical-align:top;
	    color:black;
	}

	.datum {
	    font-family:monospace;
	    text-align:right;
	    color:black
	}

	#tracking {
	    font-size: 14px;
	    font-weight: bold;
	}


    </style>

    <script>

	// labels on track button determine state
	var tracking_on_label = 'Stop Tracking';
	var tracking_off_label = 'Start Tracking';

	// sky path border
	var sky_border = 25;

	// handy shortcut
	function byId (id) {
	    return document.getElementById(id);
	}

	// separate window for plotting skypath
	var skypathwin = undefined;

	// called once after DOM is loaded
	window.onload = function() {
	    createSkyPathWin();
	    setTimeout (queryNewValues, 1000);
	}

	// close skyplotwin when main page is closed
	window.onunload = function() {
	    if (skypathwin)
		skypathwin.close();
	}

	// create separate skypathwin filled with a canvas
	function createSkyPathWin() {

	    // create a new HTML window
	    skypathwin = window.open ('', '_blank', 'width=350,height=350,scrollbars=no');
	    if (!skypathwin) {
		alert ("Please turn off Popup blocker if you want to see the Sky Path plot");
		return;
	    }
	    skypathwin.document.write ('<!DOCTYPE html><html></html>');

	    // fill with a canvas
	    var controls = '<head><title> Sky Path </title></head>';
	    controls += '<body>';
	    controls += '  <canvas id="skypath" > </canvas>';
	    controls += '</body>';
	    skypathwin.document.documentElement.innerHTML = controls;

	    // connect click handler
	    var cvs = skypathwin.document.getElementById ('skypath');
	    cvs.addEventListener ("click", onSkyPathClick);

	}

	// called when user clicks on the sky path, send Az/El as if override
	function onSkyPathClick(event) {
	    var cvs = skypathwin.document.getElementById ('skypath');
	    var rect = cvs.getBoundingClientRect();
	    var cvsw = rect.right - rect.left;
	    var cvsh = rect.bottom - rect.top;
	    var hznr = Math.min(cvsh,cvsw)/2 - sky_border;
	    var skye = event.clientX-rect.left-cvsw/2;			// right from center
	    var skyn = cvsh/2-(event.clientY-rect.top);			// up from center
	    var az = (180.0/Math.PI*Math.atan2(skye, skyn)+360) % 360;
	    var el = 90*(1-Math.hypot(skye,skyn)/hznr);

	    POSTNV ('T_Az', az);
	    POSTNV ('T_El', el);
	}
	
	// query for new values forever
	function queryNewValues() {
	    var xhr = new XMLHttpRequest();
	    xhr.onreadystatechange = function() {
		if (xhr.readyState==4 && xhr.status==200) {
		    // response is id=value pairs, one per line, end ! warning + good
		    // id is in DOM but some require special treatment.
		    var lines = xhr.responseText.replace(/\r/g,'').split('\n');
		    for (var i = 0; i < lines.length; i++) {
			console.log('getvalues line ' + i + ': ' + lines[i]);
			var nv = lines[i].trim().split('=');
			if (nv.length != 2)
			    continue;
			var id = byId (nv[0]);
			if (nv[0] == 'T_TLE' || nv[0] == 'new_TLE') {
			    console.log('getvalues line ' + (i+1) + ': ' + lines[i+1]);
			    console.log('getvalues line ' + (i+2) + ': ' + lines[i+2]);
			    id.value = nv[1] + '\n' + lines[i+1] + '\n' + lines[i+2];
			    i += 2;
			} else if (nv[0] == 'skypath') {
			    plotSkyPath (nv[1]);
			} else if (nv[0] == 'tracking') {
			    setTrackingButton (nv[1] == 'On' ? tracking_on_label : tracking_off_label);
			} else if (nv[0] == 'IP') {
			    setNewIP (nv[1]);
			} else if (nv[0] == 'GPS_Enable') {
			    setGPSEnable(nv[1]);
			} else if (nv[0] == 'SS_Save') {
			    setSSSave(nv[1]);
			} else {
			    var l = nv[1].length;
			    if (nv[1].substr(l-1) == '!') {
				// warning
				id.innerHTML = nv[1].substr(0,l-1);
				id.style.color = 'red';
			    } else if (nv[1].substr(l-1) == '+') {
				// good news
				id.innerHTML = nv[1].substr(0,l-1);
				id.style.color = '#297';
			    } else {
				// normal
				id.innerHTML = nv[1];
				id.style.color = 'black';
			    }
			}
		    }

		    // repeat after a short breather
		    setTimeout (queryNewValues, 1000);
		}
	    }
	    xhr.open('GET', UniqURL('/getvalues.txt'), true);
	    xhr.send();
	}

	// handy function to POST a name=value pair
	function POSTNV (name, value) {
	    var xhr = new XMLHttpRequest();
	    xhr.open('POST', UniqURL('/'), true);
	    xhr.send(name + '=' + String(value) + '\r\n');
	}

	// handy function that modifies a URL to be unique so it voids the cache
	function UniqURL (url) {
	    return (url + '?' + (new Date()).getTime());
	}

	// plot a skypath, points are az,el;...
	function plotSkyPath (points) {
	    // ignore if not built yet
	    if (!skypathwin)
		return;

	    // render to off-screen canvas then blit to void flashing
	    var cvs = skypathwin.document.createElement ('canvas');

	    // track current window size
	    var cvsw = skypathwin.innerWidth - 20;		// ~ 20 to eliminate scroll bars
	    var cvsh = skypathwin.innerHeight - 20;
	    cvs.width = cvsw;
	    cvs.height = cvsh;
	    var ctx = cvs.getContext ('2d');

	    var hznr = Math.min(cvsw,cvsh)/2 - sky_border;	// horizon radius

	    // local function to convert az el in degrees to skypath canvas x y
	    function azel2xy (az, el) {
		var az = Math.PI*az/180;			// cw from up, rads
		var zr = hznr*(90-el)/90;			// radius in pixels
		return {
		    x: cvsw/2 + zr*Math.sin(az),		// right
		    y: cvsh/2 - zr*Math.cos(az),		// up is - 
		};
	    }

	    // cleaner looking lines if center on pixels
	    ctx.setTransform (1, 0, 0, 1, 0, 0);
	    ctx.translate (0.5, 0.5);

	    // reset background
	    ctx.strokeStyle = 'black'
	    ctx.fillStyle = '#EEE'
	    ctx.beginPath();
		ctx.rect (0, 0, cvsw-1, cvsh-1);
	    ctx.fill();
	    ctx.stroke();

	    // draw az and el lines
	    ctx.strokeStyle = '#777'
	    ctx.beginPath();
		for (var i = 1; i <= 3; i++) {
		    ctx.moveTo (cvsw/2+i*hznr/3, cvsh/2);
		    ctx.arc (cvsw/2, cvsh/2, i*hznr/3, 0, 2*Math.PI);
		}
		for (var i = 0; i < 12; i++) {
		    ctx.moveTo (cvsw/2, cvsh/2);
		    var xy = azel2xy (i*360/12, 0);
		    ctx.lineTo (xy.x, xy.y);
		}
	    ctx.stroke();

	    // label cardinal directions
	    ctx.fillStyle = '#297'
	    ctx.font = '18px Arial';
	    ctx.fillText ('N', cvsw/2-5, cvsh/2-hznr-4);
	    ctx.fillText ('E', cvsw/2+hznr+4, cvsh/2+5);
	    ctx.fillText ('S', cvsw/2-5, cvsh/2+hznr+20);
	    ctx.fillText ('W', cvsw/2-hznr-sky_border+5, cvsh/2+5);

	    // split path into individual points, we always get one even if points is empty
	    var pts = points.replace(/;$/,'').split(/;/);
	    if (pts.length > 1) {
		// path, plotted N up E right
		ctx.strokeStyle = '#22A'
		ctx.lineWidth = 2;
		ctx.beginPath();
		    for (var i = 0; i < pts.length; i++) {
			var azel = pts[i].split(/,/);
			var xy = azel2xy (azel[0], azel[1]);
			if (i == 0)
			    ctx.moveTo (xy.x, xy.y);
			else
			    ctx.lineTo (xy.x, xy.y);
		    }
		ctx.stroke();

		// label set location
		// N.B. first location will not be rise if path computed while up
		ctx.font = '14px Arial';
		ctx.fillStyle = '#22A';
		var az = pts[pts.length-1].split(/,/)[0];
		var el = (az < 90 || az > 270) ? -5 : -12;
		var xy = azel2xy (az, el);
		ctx.fillText ('S', xy.x-5, xy.y);
	    }

	    // plot target, get loc from text fields
	    var taz = parseFloat(byId('T_Az').innerHTML);
	    var tel = parseFloat(byId('T_El').innerHTML);
	    if (tel >= 0) {
		var xy = azel2xy (taz, tel);
		ctx.fillStyle = '#FD0000';
		var r = 5;
		var px = cvsw/2-hznr-sky_border+r+5, py = cvsh/2-hznr-8-r;
		ctx.beginPath();
		    ctx.moveTo (xy.x + r, xy.y);
		    ctx.arc (xy.x, xy.y, r, 0, 2*Math.PI);
		    ctx.moveTo (px + r, py);
		    ctx.arc (px, py, r, 0, 2*Math.PI);
		ctx.fill();
		ctx.fillStyle = 'black'
		ctx.font = '14px Arial';
		ctx.fillText ('Target', px+2*r, py+r);
	    }

	    // plot sensor position, get loc from text fields
	    var saz = parseFloat(byId('SS_Az').innerHTML);
	    var sel = parseFloat(byId('SS_El').innerHTML);
	    if (sel >= -10) {	// allow for symbol size
		var xy = azel2xy (saz, sel);
		ctx.strokeStyle = '#297'
		var r = 8;
		var px = cvsw/2-hznr-sky_border+r+5, py = cvsh/2+hznr+(20-r);
		ctx.beginPath();
		    ctx.moveTo (xy.x + r, xy.y);
		    ctx.arc (xy.x, xy.y, r, 0, 2*Math.PI);
		    ctx.moveTo (px + r, py);
		    ctx.arc (px, py, r, 0, 2*Math.PI);
		ctx.stroke();
		ctx.fillStyle = 'black'
		ctx.font = '14px Arial';
		ctx.fillText ('Sensor', px+2*r, py+r);
	    }

	    // display pointing error from text fields
	    // haversine form is better than law of cosines for small separations
	    var tazr = taz*Math.PI/180;
	    var telr = tel*Math.PI/180;
	    var sazr = saz*Math.PI/180;
	    var selr = sel*Math.PI/180;
	    // var sep = 180/Math.PI*acos(sin(telr)*sin(selr) + cos(telr)*cos(selr)*sin(tazr-sazr));
	    var delel = telr - selr;
	    var delaz = tazr - sazr;
	    var a = Math.sin(delel/2)*Math.sin(delel/2) +
	    		Math.cos(telr) * Math.cos(selr) * Math.sin(delaz/2) * Math.sin(delaz/2);
	    var sep = 2*180/Math.PI*Math.atan2(Math.sqrt(a), Math.sqrt(1-a));
	    if (!isNaN(sep)) {
		ctx.fillStyle = 'black'
		ctx.font = '14px Arial';
		ctx.fillText ('Error: ' + sep.toFixed(1) + '\u00B0', cvsw/2+hznr+sky_border-90, cvsh/2-hznr-8);
	    }

	    // blit onto screen
	    var spcvs = skypathwin.document.getElementById ('skypath');
	    spcvs.width = cvsw;
	    spcvs.height = cvsh;
	    var spctx = spcvs.getContext ('2d');
	    spctx.drawImage (cvs, 0, 0);
	}

	// called when op wants to read a TLE from a file
	function handleFileSelect(file) {

	    // get target
	    var target = byId('target_name').value.trim();
	    if (!target || target.length<1) {
		alert ('Please enter name of target in file');
		return;
	    }

	    // use FileReader to read file
	    var reader = new FileReader();

	    // define callback called after reader is triggered
	    reader.onload = function(event) {
		var fr = event.target;                      // FileReader
		var text = fr.result;                       // file text as string

		// scan file looking for named target, allowing a very generous match
		var target_scrubbed = target.replace(/\W/g,'').toUpperCase();
		var lines = text.replace(/\r/g,'').split(/\n/);
		for (i = 0; i < lines.length; i++) {
		    var line = lines[i].trim();
		    var line_scrubbed = line.replace(/\W/g,'').toUpperCase();
		    if (line_scrubbed.indexOf(target_scrubbed) >= 0) {
			if (i < lines.length-2) {
			    var l1 = lines[i+1].trim();
			    var l2 = lines[i+2].trim();
			    var candidate = line + '\n' + l1 + '\n' + l2;
			    var telok = validateTLE(candidate);
			    if (telok != null)
				alert ('Found "' + target + '" in ' + file.name + ' but ' + telok);
			    else
				byId('new_TLE').value = candidate;
			} else
			    alert ('Found "' + target + '" in ' + file.name + ' but not followed by a valid TLE');
			return;
		    }
		}
		alert ('Can not find "' + target + '" in file ' + file.name);

	    };

	    // read file, triggers onload when complete
	    reader.readAsText(file);
	}



	// send new value in response to operator typing an override value.
	function onOvd(event) {
	    if (event.keyCode == 13) {
		var oid = event.target.id;
		var nam = oid.replace ('_Ovd', '');
		var vid = byId(nam);
		if (vid) {
		    var val = event.target.value.trim();
		    POSTNV (nam, val);
		}
	    }
	}

	// called when op wants to look for a target at a remote web site
	function querySite(event) {
	    var url = event.target.value;
	    var sat = byId('target_name').value.trim();
	    console.log(url);
	    POSTNV ('querySite', sat + ',' + url);
	}

	// called when op wants to upload a new TLE
	function onUploadTLE() {
	    var tid = byId('new_TLE');
	    var newtle = tid.value.trim();
	    var telok = validateTLE(newtle);
	    if (telok != null)
		alert (telok);
	    else {
		tid.value = newtle;		// show trimmed version
		POSTNV ('T_TLE', newtle);
	    }
	}

	// called to enable GPS
	function onGPSEnable() {
	    POSTNV ('GPS_Enable', 'true');
	}

	// called to save Sensor calibration to EEPROM
	function onSSSave() {
	    POSTNV ('SS_Save', 'true');
	}

	// called to upload a new IP, either with Set (k==0) or by typing Enter (k==1)
	function onIP (k,event) {
	    if (k && event.keyCode != 13)
		return;	// wait for Enter
	    var ip = byId ('IP').value.trim();
	    var octets = ip.split(/\./);
	    for (var i = 0; i < octets.length; i++) {
		var o = octets[i];
		if (!o.match(/^\d+$/))
		    break;
		var v = parseInt(o);
		if (v < 0 || v > 255)
		    break;
	    }
	    if (octets.length != 4 || i < octets.length)
		alert (ip + ': not a valid IP address format');
	    else
		POSTNV ('IP', ip);
	}

	// called to display the current IP.  N.B. leave IP text alone if it or Set currently has focus
	function setNewIP (ip) {
	    var ip_text = byId('IP')
	    var ip_set  = byId('IP-set')
	    var focus = document.activeElement;
	    if (focus != ip_text && focus != ip_set)
		ip_text.value = ip;
	}

	// called to set visibility of GPS_Enable
	function setGPSEnable (whether) {
	    var gid = byId ('GPS_Enable');
	    gid.style.visibility = (whether == 'true') ? 'visible' : 'hidden';
	}

	// called to set visibility of SS_Save
	function setSSSave (whether) {
	    var sid = byId ('SS_Save');
	    sid.style.visibility = (whether == 'true') ? 'visible' : 'hidden';
	}

	// send command to start tracking
	function commandStartTracking() {
	    var xhr = new XMLHttpRequest();
	    xhr.open('POST', UniqURL('/start'), true);
	    xhr.send();
	}

	// send command to stop tracking
	function commandStopTracking() {
	    var xhr = new XMLHttpRequest();
	    xhr.open('POST', UniqURL('/stop'), true);
	    xhr.send();
	}

	// the Run/Stop tracking button was clicked, label determines action
	function onTracking() {
	    var tb = byId('tracking');
	    // just issue command, let next getvalues update button appearance
	    if (tb.innerHTML == tracking_off_label)
		commandStartTracking();
	    else
		commandStopTracking();
	}

	// given one of tracking_on/off_label, set the tracking button appearance
	function setTrackingButton (label) {
	    var tb = byId('tracking');
	    if (label == tracking_off_label) {
		tb.innerHTML = label;
		tb.style.color = 'black';
	    } else if (label == tracking_on_label) {
		tb.innerHTML = label;
		tb.style.color = 'red';
	    } else {
		tb.innerHTML = '?';
		tb.style.color = 'blue';
	    }
	}

	// send command to reboot the Ardunio then reload our page after a short while 
	function onReboot() {
	    if (confirm('Are you sure you want to reboot the Tracker?')) {

		var xhr = new XMLHttpRequest();
		xhr.open ('POST', UniqURL('/reboot'), true);
		xhr.send ();

		byId ('op_message').style.color = 'red';

		function reloadMessage (n) {
		    var msg = 'This page will reload in ' + n + ' second' + ((n == 1) ? '' : 's');
		    byId ('op_message').innerHTML = msg;
		    if (n == 0)
			location.reload();
		    else
			setTimeout (function() {reloadMessage(n-1);}, 1000);
		}
		reloadMessage(5);
	    }
	}

	// return why the given text appears not to be a valid TLE, else null
	function validateTLE (text) {
	    var lines = text.replace(/\r/g,'').split('\n');
	    if (lines.length != 3)
		return ('TLE must be exactly 3 lines');
	    var l1 = lines[0].trim();
	    if (l1.length < 1)
		return ('Missing name on line 1');
	    var l2 = lines[1].trim();
	    if (!TLEChecksum(l2))
		return ('Invalid checksum on line 2');
	    var l3 = lines[2].trim();
	    if (!TLEChecksum(l3))
		return ('Invalid checksum on line 3');
	    return null;	// ok!
	}

	// return whether the given line has a valid TLE checksum
	function TLEChecksum (line) {
	    if (line.length < 69)
		return false;
	    var scrub = line.replace(/[^\d-]/g,'').replace(/-/g,'1');	// only digits and - is worth 1
	    var sl = scrub.length;
	    var sum = 0;
	    for (var i = 0; i < sl-1; i++)				// last char is checksum itself
		sum += parseInt(scrub.charAt(i));
	    return ((sum%10) == parseInt(scrub.charAt(sl-1)));
	}

	// called when op wants to erase the TLE text entry
	function onEraseTLE() {
	    byId ('new_TLE').value = '';
	}


    </script> 

</head>

<body>

    <!-- table floats left, so this actually centers what remains, ie, the skypath canvas -->
    <center>

    <table>
	<tr>
	    <td id='title-row' colspan='7' >
		<table style='border:none;' width='100%'>
		    <tr>
			<td width='25%' style='text-align:left; border:none' >
			    IP:
			    <input id='IP' type='text' onkeypress='onIP(1,event)' size='14' > </input>
			    <button id='IP-set' onclick='onIP(0,event)'>Change</button
			</td>
			<td width='50%' style='border:none' >
			    <label id='title-label' title='Version 2025081202' >Autonomous Satellite Tracker</label>
			    <br>
			    <label id='title-attrib' > by
				<a target='_blank' href='http://www.clearskyinstitute.com/ham'>WB&Oslash;OEW</a>
			    </label>
			</td>
			<td width='25%' style='text-align:right; border:none' >
			    <button id='reboot_b' onclick='onReboot()'> Reboot Tracker </button>
			    <br>
			    Up: <label id='uptime' style='padding:10px;'  ></label>
			</td>
		    </tr>
		    <tr>
			<td colspan='3' width='100%' style='text-align:center; border:none'>
			    <label id='op_message' > Hello </label>
			</td>
		    </tr>
		</table>
	    </td>
	</tr>

	<tr class='major-section' >
	    <td colspan='7' style='text-align:left; border-bottom-style:none; font-weight:bold' >
		<table width='100%' style='border:none'>
		    <tr>
			<td width='33%' style='text-align:left; border:none; font-weight:bold' >
			    Loaded TLE:
			</td>
			<td width='33%' style='text-align:center; border:none' >
			    <button id='tracking' onclick='onTracking()' > </button>
			</td>
			<td width='33%' style='border-style:none' >
			</td>
		    </tr>
		</table>
	    </td>
	</tr>

	<tr>
	    <td colspan='7' style='text-align:center; border:none; padding-bottom:10px' >
		<textarea id='T_TLE' class='TLE-display' rows='3' cols='69' readonly>
		</textarea>
	    </td>
	</tr>

	<tr>
	    <td colspan='7' style='text-align:left; border: none; ' >
		<table width='100%' style='border:none'>
		    <tr>
			<td style='text-align:left; border:none; font-weight:bold' >
			    Paste next TLE or look up
			    <input id='target_name' type='text' size='8' > </input>
			    at
					<button onclick='querySite(event)' value='http://www.amsat.org/tle/dailytle.txt'>AMSAT</button>
			    <button onclick='querySite(event)' value='http://192.168.1.190/esats.txt'>Local Server</button>
			    or in
			    <input type='file' id='filesel' onchange='handleFileSelect(this.files[0])' />
			    <button onclick='onUploadTLE()'>Upload</button>
			</td>
			<td style='text-align:right; border:none;' >
			    <button onclick='onEraseTLE()'>Erase</button>
			</td>
		    </tr>
		</table>
	    </td>
	</tr>

	<tr>
	    <td colspan='7' style='text-align:center; border-top-style:none; border-bottom-style:none; padding-bottom:10px' >
		<textarea id='new_TLE' class='TLE-entry' rows='3' cols='69' >
		</textarea>
	    </td>
	</tr>

	<tr class='major-section' >
	    <th class='group-head' > Subsystem </th>
	    <th> Parameter </th>
	    <th> Value </th>
	    <th> Override </th>
	    <th> Parameter </th>
	    <th> Value </th>
	    <th> Override </th>
	</tr>



	<tr class='minor-section even-row' >
	    <th rowspan='8' class='group-head' >
	    	Target
		<br>
		<label id='T_Status'></label>
	    </th>

	    <td class='datum-label' > Azimuth, degrees E of N </td>
	    <td id='T_Az' class='datum' > </td>
	    <td>
		<input id='T_Az_Ovd' type='text' onkeypress='onOvd(event)' class='override' >
		</input>
	    </td>

	    <td class='datum-label' > Next Rise in H:M:S</td>
	    <td id='T_NextRise' class='datum' > </td>
	    <td></td>
	</tr>
	<tr class='odd-row' >
	    <td class='datum-label' > Elevation, degrees Up </td>
	    <td id='T_El' class='datum' > </td>
	    <td>
		<input id='T_El_Ovd' type='text' onkeypress='onOvd(event)' class='override' >
		</input>
	    </td>

	    <td class='datum-label' > Next Rise Azimuth</td>
	    <td id='T_RiseAz' class='datum' > </td>
	    <td></td>
	</tr>
	<tr class='even-row' >
	    <td class='datum-label' > TLE age, days </td>
	    <td id='T_Age' class='datum' > </td>
	    <td></td>

	    <td id='T_NextTrans_l' class='datum-label' > Next Transit in </td>
	    <td id='T_NextTrans' class='datum' > </td>
	    <td></td>
	</tr>
	<tr class='odd-row' >
	    <td class='datum-label' > Range, km </td>
	    <td id='T_Range' class='datum' > </td>
	    <td></td>

	    <td id='T_TransAz_l' class='datum-label' > Next Transit Azimuth </td>
	    <td id='T_TransAz' class='datum' > </td>
	    <td></td>
	</tr>
	<tr class='even-row' >
	    <td class='datum-label' > Range rate, m/s</td>
	    <td id='T_RangeR' class='datum' > </td>
	    <td></td>

	    <td id='T_TransEl_l' class='datum-label' > Next Transit Elevation </td>
	    <td id='T_TransEl' class='datum' > </td>
	    <td></td>
	</tr>
	<tr class='odd-row' >
	    <td class='datum-label' > Doppler, kHz @ 144 MHz</td>
	    <td id='T_VHFDoppler' class='datum' > </td>
	    <td></td>

	    <td class='datum-label' > Next Set in </td>
	    <td id='T_NextSet' class='datum' > </td>
	    <td></td>
	</tr>
	<tr class='even-row' >
	    <td class='datum-label' > Doppler, kHz @ 440 MHz</td>
	    <td id='T_UHFDoppler' class='datum' > </td>
	    <td></td>

	    <td class='datum-label' > Next Set Azimuth</td>
	    <td id='T_SetAz' class='datum' > </td>
	    <td></td>
	</tr>
	<tr class='odd-row' >
	    <td class='datum-label' > Sunlit</td>
	    <td id='T_Sunlit' class='datum' > </td>
	    <td></td>

	    <td id='T_Up_l' class='datum-label' > Next pass duration </td>
	    <td id='T_Up' class='datum' > </td>
	    <td></td>
	</tr>


	<tr class='minor-section even-row' >
	    <th rowspan='4' class='group-head' >
	    	Spatial sensor
		<br>
		<label id='SS_Status'></label>
		<br>
		<button id='SS_Save' onclick='onSSSave()' > Save Cal </button>
	    </th>

	    <td class='datum-label' > Azimuth, degrees E of N </td>
	    <td id='SS_Az' class='datum' > </td>
	    <td></td>

	    <td class='datum-label' > System status, 0 .. 3 </td>
	    <td id='SS_SStatus' class='datum' > </td>
	    <td></td>
	</tr>
	<tr class='odd-row' >
	    <td class='datum-label' > Elevation, degrees Up </td>
	    <td id='SS_El' class='datum' > </td>
	    <td></td>

	    <td class='datum-label' > Gyro status </td>
	    <td id='SS_GStatus' class='datum' > </td>
	    <td></td>
	</tr>
	<tr class='even-row' >
	    <td class='datum-label' > Temperature, degrees C </td>
	    <td id='SS_Temp' class='datum' > </td>
	    <td></td>

	    <td class='datum-label' > Magnetometer status </td>
	    <td id='SS_MStatus' class='datum' > </td>
	    <td></td>
	</tr>
	<tr class='odd-row' >
	    <td class='datum-label' ></td>
	    <td id='SS_XXX' class='datum' > </td>
	    <td></td>

	    <td class='datum-label' > Accelerometer status </td>
	    <td id='SS_AStatus' class='datum' > </td>
	    <td></td>
	</tr>




	<tr class='minor-section even-row' >
	    <th rowspan='4' class='group-head' >
	    	GPS
		<br>
		<label id='GPS_Status'></label>
		<br>
		<button id='GPS_Enable' onclick='onGPSEnable()' > Enable </button>
	    </th>

	    <td class='datum-label' > UTC, H:M:S </td>
	    <td id='GPS_UTC' class='datum' > </td>
	    <td>
		<input id='GPS_UTC_Ovd' type='text' onkeypress='onOvd(event)' class='override' >
		</input>
	    </td>

	    <td class='datum-label' > Altitude, m </td>
	    <td id='GPS_Alt' class='datum' > </td>
	    <td>
		<input id='GPS_Alt_Ovd' type='text' onkeypress='onOvd(event)' class='override' >
		</input>
	    </td>
	</tr>
	<tr class='odd-row' >
	    <td class='datum-label' > Date, Y M D </td>
	    <td id='GPS_Date' class='datum' > </td>
	    <td>
		<input id='GPS_Date_Ovd' type='text' onkeypress='onOvd(event)' class='override' >
		</input>
	    </td>

	    <td class='datum-label' > Mag decl, true - mag </td>
	    <td id='GPS_MagDecl' class='datum' > </td>
	    <td></td>
	</tr>
	<tr class='even-row' >
	    <td class='datum-label' > Latitude, degrees +N </td>
	    <td id='GPS_Lat' class='datum' > </td>
	    <td>
		<input id='GPS_Lat_Ovd' type='text' onkeypress='onOvd(event)' class='override' >
		</input>
	    </td>

	    <td class='datum-label' > HDOP, ~1 .. 20 </td>
	    <td id='GPS_HDOP' class='datum' > </td>
	    <td></td>
	</tr>
	<tr class='odd-row' >
	    <td class='datum-label' > Longitude, degrees +E </td>
	    <td id='GPS_Long' class='datum' > </td>
	    <td>
		<input id='GPS_Long_Ovd' type='text' onkeypress='onOvd(event)' class='override' >
		</input>
	    </td>

	    <td class='datum-label' > N Satellites </td>
	    <td id='GPS_NSat' class='datum' > </td>
	    <td></td>
	</tr>



	<!-- N.B. beware that some ID's are used in a match in onOvd(event) -->
	<tr class='minor-section even-row ' >
	    <th rowspan='3' class='group-head' >
	    	Gimbal
		<br>
		<label id='G_Status'></label>
	    </th>

	    <td class='datum-label' > Servo 1 pulse length, &micro;s </td>
	    <td id='G_Mot1Pos' class='datum' > </td>
	    <td>
		<input id='G_Mot1Pos_Ovd' type='text' onkeypress='onOvd(event)' class='override' >
		</input>
	    </td>

	    <td class='datum-label' > Servo 2 pulse length, &micro;s </td>
	    <td id='G_Mot2Pos' class='datum' > </td>
	    <td>
		<input id='G_Mot2Pos_Ovd' type='text' onkeypress='onOvd(event)' class='override' >
		</input>
	    </td>
	</tr>
	<tr class='odd-row' >
	    <td class='datum-label' > Servo 1 minimum pulse </td>
	    <td id='G_Mot1Min' class='datum' > </td>
	    <td>
		<input id='G_Mot1Min_Ovd' type='text' onkeypress='onOvd(event)' class='override' >
		</input>
	    </td>

	    <td class='datum-label' > Servo 2 minimum pulse </td>
	    <td id='G_Mot2Min' class='datum' > </td>
	    <td>
		<input id='G_Mot2Min_Ovd' type='text' onkeypress='onOvd(event)' class='override' >
		</input>
	    </td>
	</tr>
	<tr class='even-row' >
	    <td class='datum-label' > Servo 1 maximum pulse </td>
	    <td id='G_Mot1Max' class='datum' > </td>
	    <td>
		<input id='G_Mot1Max_Ovd' type='text' onkeypress='onOvd(event)' class='override' >
		</input>
	    </td>

	    <td class='datum-label' > Servo 2 maximum pulse </td>
	    <td id='G_Mot2Max' class='datum' > </td>
	    <td>
		<input id='G_Mot2Max_Ovd' type='text' onkeypress='onOvd(event)' class='override' >
		</input>
	    </td>
	</tr>

    </table>

</body>
</html>
)_raw_html_";

/* send the main page, in turn it will send us commands using XMLHttpRequest
 */
void Webpage::sendMainPage (WiFiClient &client)
{
	sendHTMLHeader (client);
        client.print(FPSTR(main_page));
}

/* send HTTP header for plain text content
 */
void Webpage::sendPlainHeader (WiFiClient &client)
{
	client.print (F(
	    "HTTP/1.0 200 OK \r\n"
	    "Content-Type: text/plain \r\n"
	    "Connection: close \r\n"
	    "\r\n"
	));
}

/* send HTTP header for html content
 */
void Webpage::sendHTMLHeader (WiFiClient &client)
{
	client.print (F(
	    "HTTP/1.0 200 OK \r\n"
	    "Content-Type: text/html \r\n"
	    "Connection: close \r\n"
	    "\r\n"
	));
}

/* send empty response
 */
void Webpage::sendEmptyResponse (WiFiClient &client)
{
	client.print (F(
	    "HTTP/1.0 200 OK \r\n"
	    "Content-Type: text/html \r\n"
	    "Connection: close \r\n"
	    "Content-Length: 0 \r\n"
	    "\r\n"
	));
}

/* send back error 404 when requested page not found.
 * N.B. important for chrome otherwise it keeps asking for favicon.ico
 */
void Webpage::send404Page (WiFiClient &client)
{
	Serial.println ("Sending 404");
	client.print (F(
	    "HTTP/1.0 404 Not Found \r\n"
	    "Content-Type: text/html \r\n"
	    "Connection: close \r\n"
	    "\r\n"
	    "<html> \r\n"
	    "<body> \r\n"
	    "<h2>404: Not found</h2>\r\n \r\n"
	    "</body> \r\n"
	    "</html> \r\n"
	));
}

/* reboot
 */
void Webpage::reboot()
{
	resetWatchdog();
	Serial.println("rebooting");
	delay(5000);
	ESP.restart();
}
