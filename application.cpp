#include <stdint.h>
#include <string.h>

#include <Wire.h>

#include "raat.hpp"
#include "raat-buffer.hpp"

#include "raat-oneshot-timer.hpp"
#include "raat-oneshot-task.hpp"
#include "raat-task.hpp"

#include "http-get-server.hpp"

typedef enum _eState {
    eState_Setup,
    eState_WaitForEmergencyPower,
    eState_WaitForFloatSwitch,
    eState_WaitForStart,
    eState_Started
} eState;

static HTTPGetServer s_server(NULL);
static const raat_devices_struct * s_pDevices = NULL;
static eState s_State = eState_Setup;

static unsigned long s_lastStartPressMs = 0U;
static bool s_bDoorClosed = true;
static bool s_bDoorOverridden = false;

static void open_door(bool open)
{
    s_bDoorClosed = !open;
    s_pDevices->pSlidingDoorMaglock->set(!open);
}

static void send_standard_erm_response()
{
    s_server.set_response_code_P(PSTR("200 OK"));
    s_server.set_header_P(PSTR("Access-Control-Allow-Origin"), PSTR("*"));
    s_server.finish_headers();
}

static void get_epower_status(char const * const url)
{
    if (url)
    {
        send_standard_erm_response();
    }
    s_server.add_body_P(s_pDevices->pEmergencyPower->state() ? PSTR("OPEN\r\n\r\n") : PSTR("CLOSED\r\n\r\n"));
}

static void get_tank_status(char const * const url)
{
    if (url)
    {
        send_standard_erm_response();
    }
    s_server.add_body_P(s_pDevices->pFloatSwitch->state() ? PSTR("OPEN\r\n\r\n") : PSTR("CLOSED\r\n\r\n"));
}

static void get_started_status(char const * const url)
{
    if (url)
    {
        send_standard_erm_response();
    }
    s_server.add_body_P(s_State == eState_Started ? PSTR("OPEN\r\n\r\n") : PSTR("CLOSED\r\n\r\n"));
}

static void open_door_url_handler(char const * const url)
{
    if (url)
    {
        send_standard_erm_response();
    }
    open_door(true);
    s_bDoorOverridden = true;
}

static void close_door_url_handler(char const * const url)
{
    if (url)
    {
        send_standard_erm_response();
    }
    open_door(false);
}

static void start_game(char const * const url)
{
    s_bDoorOverridden = false;
    if (url)
    {
        send_standard_erm_response();
    }
    s_State = eState_WaitForEmergencyPower;
}

static void setup_game(char const * const url)
{
    s_bDoorOverridden = false;
    if (url)
    {
        send_standard_erm_response();
    }
    s_State = eState_Setup;
}

static const char EPOWER_STATUS_URL[] PROGMEM = "/epower/status";
static const char TANK_STATUS_URL[] PROGMEM = "/tank/status";
static const char START_BUTTON_STATUS_URL[] PROGMEM = "/start/status";
static const char DOOR_OPEN_URL[] PROGMEM = "/door/open";
static const char DOOR_CLOSE_URL[] PROGMEM = "/door/close";
static const char START_GAME[] PROGMEM = "/game/start";
static const char SETUP_GAME[] PROGMEM = "/game/setup";

static http_get_handler s_handlers[] = 
{
    {EPOWER_STATUS_URL, get_epower_status},
    {TANK_STATUS_URL, get_tank_status},
    {START_BUTTON_STATUS_URL, get_started_status},
    {DOOR_OPEN_URL, open_door_url_handler},
    {DOOR_CLOSE_URL, close_door_url_handler},
    {START_GAME, start_game},
    {SETUP_GAME, setup_game},
    {"", NULL}
};

void ethernet_packet_handler(char * req)
{
    s_server.handle_req(s_handlers, req);
}

char * ethernet_response_provider()
{
    return s_server.get_response();
}

void raat_custom_setup(const raat_devices_struct& devices, const raat_params_struct& params)
{
    (void)params;
    s_pDevices = &devices;
    raat_logln_P(LOG_APP, PSTR("Waiting for emergency power..."));
}

static void debug_task_fn(RAATTask& task, void * pTaskData)
{
    (void)task; (void)pTaskData;
    raat_logln_P(LOG_APP, PSTR("Maglock: %s, State: %d"),
        s_bDoorClosed ? "On" : "Off",
        (int)s_State
    );
}
static RAATTask s_debug_task(1000, debug_task_fn);

void raat_custom_loop(const raat_devices_struct& devices, const raat_params_struct& params)
{
    (void)devices; (void)params;

    bool bEmergencyPowerActivated = devices.pEmergencyPower->check_low_and_clear();
    bool bEmergencyPowerDeactivated = devices.pEmergencyPower->check_high_and_clear();

    bool bFuelTankFilled = devices.pFloatSwitch->check_low_and_clear();

    bool bStartButtonPressed = devices.pStartButton->check_low_and_clear();

    switch(s_State)
    {
    case eState_Setup:
        devices.pSSR1->set(!devices.pEmergencyPower->state());
        devices.pSSR2->set(!devices.pFloatSwitch->state());
        if (!s_bDoorOverridden)
        {
            open_door(false);
        }
        if (devices.pStartButton->state() == false)
        {
            devices.pSSR1->set(false);
            devices.pSSR2->set(false);
            start_game(NULL);
        }
        break;

    case eState_WaitForEmergencyPower:
        if (bEmergencyPowerActivated)
        {
            devices.pSSR1->set(true);
            s_State = eState_WaitForFloatSwitch;
            raat_logln_P(LOG_APP, PSTR("Got emergency power"));
            raat_logln_P(LOG_APP, PSTR("Waiting for float switch..."));
        }
        break;

    case eState_WaitForFloatSwitch:
        #if 0
        if (bEmergencyPowerDeactivated)
        {
            devices.pSSR1->set(false);
            s_State = eState_WaitForEmergencyPower;
            raat_logln_P(LOG_APP, PSTR("Lost emergency power"));
            raat_logln_P(LOG_APP, PSTR("Waiting..."));   
        }
        else
        #endif
        if (bFuelTankFilled)
        {
            devices.pSSR2->set(true);
            s_State = eState_WaitForStart;
            raat_logln_P(LOG_APP, PSTR("Got float switch"));
            raat_logln_P(LOG_APP, PSTR("Waiting for start..."));
        }
        break;

    case eState_WaitForStart:
        #if 0
        if (bEmergencyPowerDeactivated)
        {
            devices.pSSR1->set(false);
            devices.pSSR2->set(false);
            s_State = eState_WaitForEmergencyPower;
            raat_logln_P(LOG_APP, PSTR("Lost emergency power!"));
        }
        else
        #endif
        if (bStartButtonPressed)
        {
            s_lastStartPressMs = millis();
            raat_logln_P(LOG_APP, PSTR("Start pressed (counting)"));
        }

        if (devices.pStartButton->state() == false)
        {
            if ((millis()- s_lastStartPressMs) >= params.pStartButtonPressTime->get())
            {
                s_State = eState_Started;
                open_door(true);
                devices.pSSR1->set(true); // Keep UV on for the cool
                raat_logln_P(LOG_APP, PSTR("Got start."));
            }
        }
        break;

    case eState_Started:
        if (bEmergencyPowerDeactivated)
        {
            raat_logln_P(LOG_APP, PSTR("Lost emergency power!"));
        }
        break;
    }
    //s_debug_task.run();
    (void)s_debug_task;
}
