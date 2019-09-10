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
    eState_WaitForEmergencyPower,
    eState_WaitForFloatSwitch,
    eState_WaitForStart,
    eState_Started
} eState;

static HTTPGetServer s_server(true);
static const raat_devices_struct * s_pDevices = NULL;
static eState s_State = eState_WaitForEmergencyPower;

static unsigned long s_lastStartPressMs = 0U;
static bool s_bDoorIsAuto = true;
static bool s_bDoorClosed = true;

static void open_door(bool open)
{
    s_pDevices->pSlidingDoorMaglock->set(!open);
}

static void handle_maglock_state(void)
{
    if (s_bDoorIsAuto)
    {
        open_door(!s_bDoorClosed);
    }
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
    s_bDoorClosed = false;
    if (url)
    {
        send_standard_erm_response();
    }
    open_door(true);
    s_bDoorIsAuto = false;
}

static void close_door_url_handler(char const * const url)
{
    s_bDoorClosed = true;
    if (url)
    {
        send_standard_erm_response();
    }
    open_door(false);
    s_bDoorIsAuto = false;
}

static void set_maglock_auto(char const * const url)
{
    if (url)
    {
        send_standard_erm_response();
    }
    s_bDoorIsAuto = true;
}

static const char EPOWER_STATUS_URL[] PROGMEM = "/epower/status";
static const char TANK_STATUS_URL[] PROGMEM = "/tank/status";
static const char START_BUTTON_STATUS_URL[] PROGMEM = "/start/status";
static const char DOOR_OPEN_URL[] PROGMEM = "/door/open";
static const char DOOR_CLOSE_URL[] PROGMEM = "/door/close";
static const char DOOR_AUTO_URL[] PROGMEM = "/door/auto";

static http_get_handler s_handlers[] = 
{
    {EPOWER_STATUS_URL, get_epower_status},
    {TANK_STATUS_URL, get_tank_status},
    {START_BUTTON_STATUS_URL, get_started_status},
    {DOOR_OPEN_URL, open_door_url_handler},
    {DOOR_CLOSE_URL, close_door_url_handler},
    {DOOR_AUTO_URL, set_maglock_auto},
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
    raat_logln_P(LOG_APP, PSTR("Maglock: %s, State: %d, Auto?: %c"),
        s_bDoorClosed ? "On" : "Off",
        (int)s_State,
        s_bDoorIsAuto ? 'Y' : 'N'
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
        if (bEmergencyPowerDeactivated)
        {
            devices.pSSR1->set(false);
            s_State = eState_WaitForEmergencyPower;
            raat_logln_P(LOG_APP, PSTR("Lost emergency power"));
            raat_logln_P(LOG_APP, PSTR("Waiting..."));   
        }
        else if (bFuelTankFilled)
        {
            devices.pSSR2->set(true);
            s_State = eState_WaitForStart;
            raat_logln_P(LOG_APP, PSTR("Got float switch"));
            raat_logln_P(LOG_APP, PSTR("Waiting for start..."));
        }
        break;

    case eState_WaitForStart:
        if (bEmergencyPowerDeactivated)
        {
            devices.pSSR1->set(false);
            devices.pSSR2->set(false);
            s_State = eState_WaitForEmergencyPower;
            raat_logln_P(LOG_APP, PSTR("Lost emergency power!"));
        }
        else if (bStartButtonPressed)
        {
            s_lastStartPressMs = millis();
            raat_logln_P(LOG_APP, PSTR("Start pressed (counting)"));
        }

        if (devices.pStartButton->state() == false)
        {
            if ((millis()- s_lastStartPressMs) >= params.pStartButtonPressTime->get())
            {
                s_State = eState_Started;
                s_bDoorClosed = false;
                devices.pSSR1->set(false);
                raat_logln_P(LOG_APP, PSTR("Got start."));
            }
        }
        break;

    case eState_Started:
        if (bEmergencyPowerDeactivated)
        {
            s_bDoorClosed = true;
            devices.pSSR1->set(false);
            devices.pSSR2->set(false);
            s_State = eState_WaitForEmergencyPower;
            raat_logln_P(LOG_APP, PSTR("Lost emergency power!"));
        }
        break;
    }
    handle_maglock_state();
    //s_debug_task.run();
    (void)s_debug_task;
}
