/* ask op for local wifi info
 */

#include "Webpage.h"

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>

// code to handle more chars is from DL2TW


//credits for this code go to zenmanenergy (Steve Nelson) on github
unsigned char h2int(char c)
{
    if (c >= '0' && c <='9'){
        return((unsigned char)c - '0');
    }
    if (c >= 'a' && c <='f'){
        return((unsigned char)c - 'a' + 10);
    }
    if (c >= 'A' && c <='F'){
        return((unsigned char)c - 'A' + 10);
    }
    return(0);
}

String urldecode(String str)
{

    String encodedString="";
    char c;
    char code0;
    char code1;
    for (int i =0; i < str.length(); i++){
        c=str.charAt(i);
      if (c == '+'){
        encodedString+=' ';
      }else if (c == '%') {
        i++;
        code0=str.charAt(i);
        i++;
        code1=str.charAt(i);
        c = (h2int(code0) << 4) | h2int(code1);
        encodedString+=c;
      } else{

        encodedString+=c;
      }

      yield();
    }

   return encodedString;
}


/* set up an Access Point to ask operator for local wifi info, save in NV
 */
void Webpage::askWiFi()
{
	// AP info
	IPAddress ip(192,168,10,10);
	IPAddress gw(192,168,10,10);
	IPAddress nm(255,255,255,0);
	const char *ssid = "SatTrack";

	// start AP
	resetWatchdog();
	WiFi.disconnect(true);
	delay(200);
	WiFi.mode(WIFI_AP);
	if (!WiFi.softAPConfig(ip, gw, nm)) {
	    Serial.println ("Can not configure softAP");
	    return;
	}
	if (!WiFi.softAP (ssid)) {
	    Serial.println ("Can not set AP ssid");
	    return;
	}
	delay(500);
	Serial.print (F("AP IP: ")); Serial.println (WiFi.softAPIP());

	// start HTTP server 
	resetWatchdog();
	WiFiServer remoteServer(80);
	remoteServer.begin();

	// provide clients with an IP pointing back to us
	DNSServer dns;
	dns.start(53, "*", WiFi.softAPIP());

	// repeat until get station info

	int n_info = 0;
	while (n_info < 5) {
	    n_info = 0;

	    // listen for a connection
	    WiFiClient remoteClient;
	    Serial.println ("waiting for client");
	    dns.processNextRequest();
	    do {
		resetWatchdog();
		delay(100);
		remoteClient = remoteServer.available();
	    } while (!remoteClient);
	    Serial.print (F("client connected from ")); Serial.println (remoteClient.remoteIP());

	    // send the WiFI setup page
	    sendAskPage (remoteClient);

	    // look for GET down to blank line
	    char c, line[1024];
	    size_t linel = 0;
	    char *GET = NULL;
	    uint32_t to = millis();
	    while ((c = readNextClientChar (remoteClient, &to)) != 0) {
		if (!GET) {
		    if (c == '\n') {
			line[linel] = '\0';
			linel = 0;
			if (strstr (line, "GET "))
			    GET = line;
		    } else if (linel < sizeof(line)-1)
			line[linel++] = c;
		} else {
		    if (c == '\n') {
			if (linel == 1)	// just \r\n
			    break;
			linel = 0;
		    } else {
			linel++;
		    }
		}
	    }
	    remoteClient.stop();
	    Serial.println (line);

	    // abort if no GET
	    if (!GET) {
		Serial.println (line);
		Serial.println ("No GET");
		break;
	    }

	    // crack GET for desired info
	    char *wifi_id = strstr (line, "id=");
	    char *wifi_pw = strstr (line, "pw=");
	    char *wifi_ip = strstr (line, "ip=");
	    char *wifi_nm = strstr (line, "nm=");
	    char *wifi_gw = strstr (line, "gw=");

	    // TODO: check for special characters

	    // save if looks reasonable
	    char *eos;
	    if (wifi_id) {
		wifi_id += 3;
		eos = strchr (wifi_id, '&');
		if (eos) {
		    *eos = '\0';
                    String wifi_ssid1 = urldecode((String)wifi_id);
		    strncpy (nv->ssid, (char *)&wifi_ssid1[0], sizeof(nv->ssid));
		    nv->put();
		    Serial.println(nv->ssid);
		    n_info++;
		}
	    }
	    if (wifi_pw) {
		wifi_pw += 3;
		eos = strchr (wifi_pw, '&');
		if (eos) {
		    *eos = '\0';
                    String wifi_pw1 = urldecode((String)wifi_pw);
		    strncpy (nv->pw, (char *)&wifi_pw1[0], sizeof(nv->pw));
		    nv->put();
		    Serial.println(nv->pw);
		    n_info++;
		}
	    }
	    if (wifi_ip) {
		wifi_ip += 3;
		eos = strchr (wifi_ip, '&');
		if (eos) {
		    *eos = '\0';
		    nv->IP.fromString (wifi_ip);
		    nv->put();
		    Serial.println(wifi_ip);
		    n_info++;
		}
	    }
	    if (wifi_nm) {
		wifi_nm += 3;
		eos = strchr (wifi_nm, '&');
		if (eos) {
		    *eos = '\0';
		    nv->NM.fromString (wifi_nm);
		    nv->put();
		    Serial.println(wifi_nm);
		    n_info++;
		}
	    }
	    if (wifi_gw) {
		wifi_gw += 3;
		eos = strchr (wifi_gw, ' ');
		if (eos) {
		    *eos = '\0';
		    nv->GW.fromString (wifi_gw);
		    nv->put();
		    Serial.println(wifi_gw);
		    n_info++;
		}
	    }
	}

	// shut down AP
	WiFi.softAPdisconnect(true);
	delay(200);
}

static const char ask_page[] PROGMEM =  R"_raw_html_(

<!DOCTYPE html>
<html>
<head>
    <meta http-equiv='Content-Type' content='text/html; charset=UTF-8' />

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

	td {
            padding: 4px;
	}

        th {
            padding: 6px;
            border: 1px solid brown;
        }

	input {
	    width : 90%;
	}

    </style>

</head>
<body>

    <form> 
    <table>
	<tr>
	    <th colspan='2' >
		Enter local WiFi information:
	    </th>
	</tr>
	<tr>
	    <td>
		SSID:
	    </td>
	    <td>
		<input name='id'></input>
	    </td>
	</tr>
	<tr>
	    <td>
		Password:
	    </td>
	    <td>
		<input name='pw'></input>
	    </td>
	</tr>
	<tr>
	    <td>
		IP:
	    </td>
	    <td>
		<input name='ip'></input>
	    </td>
	</tr>
	<tr>
	    <td>
		Network mask:
	    </td>
	    <td>
		<input name='nm'></input>
	    </td>
	</tr>
	<tr>
	    <td>
		Gateway:
	    </td>
	    <td>
		<input name='gw'></input>
	    </td>
	</tr>
	<tr>
	    <td colspan='2' >
		<button type='submit'>Send</button>
	    </td>
    </table>
    </form>
</body>
</html>

)_raw_html_";


/* transmit the page that lets operator enter wifi info
 */
void Webpage::sendAskPage(WiFiClient &client)
{
        sendHTMLHeader (client);
        client.print (FPSTR(ask_page));

}
