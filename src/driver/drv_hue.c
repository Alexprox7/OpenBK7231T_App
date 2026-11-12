#include "../new_common.h"
#include "../new_pins.h"
#include "../new_cfg.h"
// Commands register, execution API and cmd tokenizer
#include "../cmnds/cmd_public.h"
#include "../mqtt/new_mqtt.h"
#include "../logging/logging.h"
#include "../hal/hal_pins.h"
#include "../hal/hal_wifi.h"
#include "drv_public.h"
#include "drv_local.h"
#include "drv_ssdp.h"
#include "../httpserver/new_http.h"

// Hue driver for Alexa, requiring btsimonh's SSDP to work
// Based on Tasmota approach
// The procedure is following:
// 1. first MSEARCH over UDP is done
// 2. then obk replies to MSEARCH with page details
// 3. then alexa accesses our XML pages here with GET
// 4. and can change the binary state (0 or 1) with POST

static char *buffer_out = 0;
static char *g_serial = 0;
static char *g_userID = 0;
static char *g_uid = 0;
static char *g_bridgeID = 0;
static int outBufferLen = 0;
static int stat_searchesReceived = 0;
static int stat_setupXMLVisits = 0;
// stubs
static int stat_metaServiceXMLVisits = 0;
static int stat_eventsReceived = 0;
static int stat_eventServiceXMLVisits = 0;


// ARGUMENTS: first IP, then bridgeID
const  char *hue_resp = "HTTP/1.1 200 OK\r\n"
   "HOST: 239.255.255.250:1900\r\n"
   "CACHE-CONTROL: max-age=100\r\n"
   "EXT:\r\n"
   "LOCATION: http://%s:80/description.xml\r\n"
   "SERVER: Linux/3.14.0 UPnP/1.0 IpBridge/1.24.0\r\n"  // was 1.17
   "hue-bridgeid: %s\r\n";

// ARGUMENTS: uuid
const  char *hue_resp1 = "ST: upnp:rootdevice\r\n"
  "USN: uuid:%s::upnp:rootdevice\r\n"
  "\r\n";

// ARGUMENTS: uuid and uuid
const  char *hue_resp2 =  "ST: uuid:%s\r\n"
   "USN: uuid:%s\r\n"
   "\r\n";

// ARGUMENTS: uuid
const  char *hue_resp3 = "ST: urn:schemas-upnp-org:device:basic:1\r\n"
   "USN: uuid:%s\r\n"
   "\r\n";

void DRV_HUE_Send_Advert_To(struct sockaddr_in *addr) {
	//const char *useType;

	if (g_uid == 0) {
		// not running
		return;
	}

	stat_searchesReceived++;

	if (buffer_out == 0) {
		outBufferLen = strlen(hue_resp) + 256;
		buffer_out = (char*)malloc(outBufferLen);
	}
	{
		// ARGUMENTS: first IP, then bridgeID
		snprintf(buffer_out, outBufferLen, hue_resp, HAL_GetMyIPString(), g_bridgeID);

		addLogAdv(LOG_ALL, LOG_FEATURE_HTTP, "HUE - Sending[0] %s", buffer_out);
		DRV_SSDP_SendReply(addr, buffer_out);
	}
	{
		// ARGUMENTS: uuid
		snprintf(buffer_out, outBufferLen, hue_resp1, g_uid);

		addLogAdv(LOG_ALL, LOG_FEATURE_HTTP, "HUE - Sending[1] %s", buffer_out);
		DRV_SSDP_SendReply(addr, buffer_out);
	}
	{
		// ARGUMENTS: uuid and uuid
		snprintf(buffer_out, outBufferLen, hue_resp2, g_uid, g_uid);

		addLogAdv(LOG_ALL, LOG_FEATURE_HTTP, "HUE - Sending[2] %s", buffer_out);
		DRV_SSDP_SendReply(addr, buffer_out);
	}
	{
		// ARGUMENTS: uuid
		snprintf(buffer_out, outBufferLen, hue_resp3, g_uid);

		addLogAdv(LOG_ALL, LOG_FEATURE_HTTP, "HUE - Sending[3] %s", buffer_out);
		DRV_SSDP_SendReply(addr, buffer_out);
	}
}


void HUE_AppendInformationToHTTPIndexPage(http_request_t* request, int bPreState) {
	if(bPreState)
		return;
	hprintf255(request, "<h4>HUE: searches %i, setup %i, events %i, mService %i, event %i </h4>",
		stat_searchesReceived, stat_setupXMLVisits, stat_eventsReceived, stat_metaServiceXMLVisits, stat_eventServiceXMLVisits);

}

const char *g_hue_setup_1 = "<?xml version=\"1.0\"?>"
"<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
"<specVersion>"
"<major>1</major>"
"<minor>0</minor>"
"</specVersion>"
"<URLBase>http://";
// IP ADDR HERE
const char *g_hue_setup_2 = ":80/</URLBase>"
"<device>"
"<deviceType>urn:schemas-upnp-org:device:Basic:1</deviceType>"
"<friendlyName>Amazon-Echo-HA-Bridge (";
// IP ADDR HERE
const char *g_hue_setup_3 = ")</friendlyName>"
"<manufacturer>Royal Philips Electronics</manufacturer>"
"<manufacturerURL>http://www.philips.com</manufacturerURL>"
"<modelDescription>Philips hue Personal Wireless Lighting</modelDescription>"
"<modelName>Philips hue bridge 2012</modelName>"
"<modelNumber>929000226503</modelNumber>"
"<serialNumber>";
// SERIAL here
const char *g_hue_setup_4 = "</serialNumber>"
"<UDN>uuid:";
// UID HERE
const char *g_hue_setup_5 = "</UDN>"
   "</device>"
   "</root>\r\n"
   "\r\n";

static int HUE_Setup(http_request_t* request) {
	http_setup(request, httpMimeTypeXML);
	poststr(request, g_hue_setup_1);
	poststr(request, HAL_GetMyIPString());
	poststr(request, g_hue_setup_2);
	poststr(request, HAL_GetMyIPString());
	poststr(request, g_hue_setup_3);
	poststr(request, g_serial);
	poststr(request, g_hue_setup_4);
	poststr(request, g_uid);
	poststr(request, g_hue_setup_5);
	poststr(request, NULL);

	stat_setupXMLVisits++;

	return 0;
}
static int HUE_NotImplemented(http_request_t* request) {

	http_setup(request, httpMimeTypeJson);
	poststr(request, "{}");
	poststr(request, NULL);

	return 0;
}
static int HUE_Authentication(http_request_t* request) {

	http_setup(request, httpMimeTypeJson);
	hprintf255(request, "[{\"success\":{\"username\":\"%s\"}}]",g_userID);
	poststr(request, NULL);

	return 0;
}
static int HUE_Config_Internal(http_request_t* request) {

	poststr(request, "{\"name\":\"Philips hue\",\"mac\":\"");
	// TODO: mac
	poststr(request, "\",\"dhcp\":true,\"ipaddress\":\"");
	// TODO: ip
	poststr(request, "\",\"netmask\":\"");
	// TODO: mask
	poststr(request, "\",\"gateway\":\"");
	// TODO: gw
	poststr(request, "\",\"proxyaddress\":\"none\",\"proxyport\":0,\"bridgeid\":\"");
	// TODO: bridgeid
	poststr(request, "\",\"UTC\":\"{dt\",\"whitelist\":{\"");
	// TODO: id
	poststr(request, "\":{\"last use date\":\"");
	// TODO: date
	poststr(request, "\",\"create date\":\"");
	// TODO: date
	poststr(request, "\",\"name\":\"Remote\"}},\"swversion\":\"01041302\",\"apiversion\":\"1.17.0\",\"swupdate\":{\"updatestate\":0,\"url\":\"\",\"text\":\"\",\"notify\": false},\"linkbutton\":false,\"portalservices\":false}");
	poststr(request, NULL);

	return 0;
}




static int HUE_GlobalConfig(http_request_t* request) {

	http_setup(request, httpMimeTypeJson);
	poststr(request, "{\"lights\":{");
	// TODO: lights
	poststr(request, "},\"groups\":{},\"schedules\":{},\"config\":");
	HUE_Config_Internal(request);
	poststr(request, "}");
	poststr(request, NULL);

	return 0;
}


// Alexa devices = channels HUE_FIRST_CH..HUE_LAST_CH that have a label and type Toggle (on/off)
// or Dimmer (on/off + percentage). Scripts react to the channel value with addChangeHandler.
#define HUE_FIRST_CH 10
#define HUE_LAST_CH 31
static int g_hueLast[HUE_LAST_CH + 1]; // last non-zero dimmer value, restored by a plain "turn on"
static char g_hueMac[24];

static int HUE_IsLight(int ch) {
	int t;
	if (ch < HUE_FIRST_CH || ch > HUE_LAST_CH || !CHANNEL_HasLabel(ch))
		return 0;
	t = CHANNEL_GetType(ch);
	return t == ChType_Toggle || t == ChType_Dimmer;
}
static int HUE_Bri(int ch) {
	int v = CHANNEL_Get(ch);
	if (CHANNEL_GetType(ch) != ChType_Dimmer)
		return 254;
	if (v <= 0)
		v = g_hueLast[ch] > 0 ? g_hueLast[ch] : 100;
	if (v > 100)
		v = 100;
	return (v * 254 + 50) / 100;
}
static void HUE_PrintLight(http_request_t* request, int ch) {
	// same fields as fauxmoESP, which is known to work with Echo devices
	poststr(request, "{\"type\":\"Extended color light\",\"name\":\"");
	poststr(request, CHANNEL_GetLabel(ch));
	hprintf255(request, "\",\"uniqueid\":\"%s:00:00-%02X\",\"modelid\":\"LCT015\",\"manufacturername\":\"Philips\",\"productname\":\"E4\",",
		g_hueMac, ch);
	hprintf255(request, "\"state\":{\"on\":%s,\"bri\":%d,\"xy\":[0,0],\"hue\":0,\"sat\":0,\"effect\":\"none\",\"colormode\":\"xy\",\"ct\":500,",
		CHANNEL_Get(ch) > 0 ? "true" : "false", HUE_Bri(ch));
	poststr(request, "\"mode\":\"homeautomation\",\"reachable\":true},\"capabilities\":{\"certified\":false,"
		"\"streaming\":{\"renderer\":true,\"proxy\":false}},\"swversion\":\"5.105.0.21169\"}");
}
static void HUE_PrintLights(http_request_t* request) {
	int ch, first = 1;
	poststr(request, "{");
	for (ch = HUE_FIRST_CH; ch <= HUE_LAST_CH; ch++) {
		if (!HUE_IsLight(ch))
			continue;
		hprintf255(request, "%s\"%d\":", first ? "" : ",", ch);
		HUE_PrintLight(request, ch);
		first = 0;
	}
	poststr(request, "}");
}
static void HUE_SetState(http_request_t* request, int ch) {
	const char *b = request->bodystart ? request->bodystart : "";
	const char *on = strstr(b, "\"on\"");
	const char *bri = strstr(b, "\"bri\"");
	int hasOn = 0, onVal = 0, hasBri = 0, briVal = 0;
	int v = CHANNEL_Get(ch);

	if (on && (on = strchr(on + 4, ':')) != 0) {
		on++;
		while (*on == ' ') on++;
		hasOn = 1;
		onVal = !strncmp(on, "true", 4);
	}
	if (bri && (bri = strchr(bri + 5, ':')) != 0) {
		hasBri = 1;
		briVal = atoi(bri + 1);
	}
	if (CHANNEL_GetType(ch) == ChType_Dimmer) {
		if (hasBri) {
			v = (briVal * 100 + 127) / 254; // 0..254 -> 0..100, "22%" comes back as 22
			if (v < 1) v = 1;
			if (v > 100) v = 100;
		}
		else if (hasOn && onVal) {
			v = g_hueLast[ch] > 0 ? g_hueLast[ch] : 100;
		}
		if (hasOn && !onVal)
			v = 0;
		if (v > 0)
			g_hueLast[ch] = v;
	}
	else if (hasOn) {
		v = onVal ? 1 : 0;
	}
	addLogAdv(LOG_INFO, LOG_FEATURE_HTTP, "HUE - light %i set to %i (%s)", ch, v, b);
	CHANNEL_Set(ch, v, 0);

	http_setup(request, httpMimeTypeJson);
	hprintf255(request, "[{\"success\":{\"/lights/%d/state/on\":%s}}", ch, v > 0 ? "true" : "false");
	if (hasBri)
		hprintf255(request, ",{\"success\":{\"/lights/%d/state/bri\":%d}}", ch, HUE_Bri(ch));
	poststr(request, "]");
	poststr(request, NULL);
}

// Called by the HTTP server before its own handlers; returns 1 when the request was a Hue API call.
// POST /api (pairing), GET /api/<user>, GET /api/<user>/lights[/<id>], PUT /api/<user>/lights/<id>/state
int HUE_APICall(http_request_t* request) {
	const char *p, *seg2;
	int ch;

	if (g_uid == 0) {
		// not running
		return 0;
	}
	p = request->url;
	if (strncmp(p, "api", 3))
		return 0;
	p += 3;
	if (*p == 0 || (*p == '/' && p[1] == 0)) {
		if (request->method != HTTP_POST)
			return 0;
		HUE_Authentication(request);
		return 1;
	}
	if (*p != '/')
		return 0;
	p++;
	// OBK's own REST API also lives under /api/, so only claim /api/<user>/lights... (any user name,
	// Echo devices don't always pair first) and /api/<our user>
	seg2 = strchr(p, '/');
	if (seg2 == 0) {
		if (strcmp(p, g_userID) || request->method != HTTP_GET)
			return 0;
		stat_eventsReceived++;
		http_setup(request, httpMimeTypeJson);
		poststr(request, "{\"lights\":");
		HUE_PrintLights(request);
		poststr(request, ",\"groups\":{},\"schedules\":{},\"config\":");
		HUE_Config_Internal(request);
		poststr(request, "}");
		poststr(request, NULL);
		return 1;
	}
	seg2++;
	if (strncmp(seg2, "lights", 6) || (seg2[6] != 0 && seg2[6] != '/'))
		return 0;
	stat_eventsReceived++;
	p = seg2 + 6;
	if (*p == 0 || (*p == '/' && p[1] == 0)) {
		http_setup(request, httpMimeTypeJson);
		HUE_PrintLights(request);
		poststr(request, NULL);
		return 1;
	}
	p++;
	ch = atoi(p);
	if (!HUE_IsLight(ch)) {
		http_setup(request, httpMimeTypeJson);
		poststr(request, "[{\"error\":{\"type\":3,\"address\":\"/lights\",\"description\":\"resource not available\"}}]");
		poststr(request, NULL);
		return 1;
	}
	while (*p >= '0' && *p <= '9')
		p++;
	if (!strncmp(p, "/state", 6) && request->method == HTTP_PUT) {
		HUE_SetState(request, ch);
		return 1;
	}
	http_setup(request, httpMimeTypeJson);
	HUE_PrintLight(request, ch);
	poststr(request, NULL);
	return 1;
}
// backlog startDriver SSDP; startDriver HUE
// 
void HUE_Init() {
	char tmp[64];
	unsigned char mac[8];

	WiFI_GetMacAddress((char*)mac);
	// username - 
	snprintf(tmp, sizeof(tmp), "%02X%02X%02X",  mac[3], mac[4], mac[5]);
	g_userID = strdup(tmp);
	// SERIAL - as in Tas, full 12 chars of MAC, so 5c cf 7f 13 9f 3d
	snprintf(tmp, sizeof(tmp), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	g_serial = strdup(tmp);
	// BridgeID - as in Tas, full 12 chars of MAC with FFFE inside, so 5C CF 7F FFFE 13 9F 3D
	snprintf(tmp, sizeof(tmp), "%02X%02X%02XFFFE%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	g_bridgeID = strdup(tmp);
	// uuid
	snprintf(tmp, sizeof(tmp), "f6543a06-da50-11ba-8d8f-%s", g_serial);
	g_uid = strdup(tmp);
	snprintf(g_hueMac, sizeof(g_hueMac), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);


	//HTTP_RegisterCallback("/api", HTTP_ANY, HUE_APICall);
	HTTP_RegisterCallback("/description.xml", HTTP_GET, HUE_Setup, 0);
}

