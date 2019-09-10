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
static bool s_bLinAcIsAuto = true;
static bool s_bLinAcLatch = false;

static void control_linac(bool open)
{
    s_pDevices->pLinAc->set(open ? 0 : 3);
}

static void handle_linac_movement(void)
{
    if (s_bLinAcIsAuto)
    {
        control_linac(s_bLinAcLatch);
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

static void open_linac_url_handler(char const * const url)
{
    s_bLinAcLatch = true;
    if (url)
    {
        send_standard_erm_response();
    }
    control_linac(true);
    s_bLinAcIsAuto = false;
}

static void close_linac_url_handler(char const * const url)
{
    s_bLinAcLatch = false;
    if (url)
    {
        send_standard_erm_response();
    }
    control_linac(false);
    s_bLinAcIsAuto = false;
}

static void set_linac_auto(char const * const url)
{
    s_bLinAcLatch = false;
    if (url)
    {
        send_standard_erm_response();
    }
    s_bLinAcIsAuto = true;
}

static const char EPOWER_STATUS_URL[] PROGMEM = "/epower/status";
static const char TANK_STATUS_URL[] PROGMEM = "/tank/status";
static const char START_BUTTON_STATUS_URL[] PROGMEM = "/start/status";
static const char LINAC_OPEN_URL[] PROGMEM = "/linac/open";
static const char LINAC_CLOSE_URL[] PROGMEM = "/linac/close";
static const char LINAC_AUTO_URL[] PROGMEM = "/linac/auto";

static http_get_handler s_handlers[] = 
{
    {EPOWER_STATUS_URL, get_epower_status},
    {TANK_STATUS_URL, get_tank_status},
    {START_BUTTON_STATUS_URL, get_started_status},
    {LINAC_OPEN_URL, open_linac_url_handler},
    {LINAC_CLOSE_URL, close_linac_url_handler},
    {LINAC_AUTO_URL, set_linac_auto},
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
    raat_logln_P(LOG_APP, PSTR("Door: %s, State: %d, Auto?: %c"),
        s_bLinAcLatch ? "Open" : "Closed",
        (int)s_State,
        s_bLinAcIsAuto ? 'Y' : 'N'
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
                s_bLinAcLatch = true;
                devices.pSSR1->set(false);
                raat_logln_P(LOG_APP, PSTR("Got start."));
            }
        }
        break;

    case eState_Started:
        if (bEmergencyPowerDeactivated)
        {
            s_bLinAcLatch = false;
            devices.pSSR1->set(false);
            devices.pSSR2->set(false);
            s_State = eState_WaitForEmergencyPower;
            raat_logln_P(LOG_APP, PSTR("Lost emergency power!"));
        }
        break;
    }
    handle_linac_movement();
    s_debug_task.run();
}
