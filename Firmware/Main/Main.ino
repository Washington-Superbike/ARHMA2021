#include "Main.h"
#include "CAN.h"
#include "Display.h"
#include "Precharge.h"
#include "DataLogging.h"
#include "FreeRTOS_TEENSY4.h"

static int bms_status_flag = 0;
static int bms_c_id = 0;
static int bms_c_fault = 0;
static int ltc_fault = 0;
static int ltc_count = 0;
static float cellVoltagesArr[BMS_CELLS]; // voltages starting with the first LTC
static float seriesVoltage;
static bool cellsReady;
static float thTemps[10]; // assuming only 10 thermistors
static int thermistorEnabled;
static int thermistorPresent;

static float auxiliaryBatteryVoltage = 0;

static float RPM = 0;
static float motorCurrent = 0;
static float motorControllerBatteryVoltage = 0;
static float throttle = 0;
static float motorControllerTemp = 0;
static float motorTemp = 0;
static int errorMessage = 0;
static byte controllerStatus = 0;

static byte evccEnable = 0;
static float evccVoltage = 0;
static float evccCurrent = 0;

static byte chargeFlag = 0;
static byte chargerStatusFlag = 0;
static float chargerVoltage = 0;
static float chargerCurrent = 0;
static int8_t chargerTemp = 0;

static Screen screen = {};

static MeasurementScreenData measurementData = {};
static MotorStats motorStats = {};
static MotorTemps motorTemps = {};
static CellVoltages cellVoltages = {};
static PreChargeTaskData preChargeData = {};
static BMSStatus bmsStatus = {};
static ThermistorTemps thermistorTemps = {};
static ChargerStats chargerStats = {};
static ChargeControllerStats chargeControllerStats = {};

static CANTaskData canTaskData;
static DataLoggingTaskData dataLoggingTaskData;

static CSVWriter motorTemperatureLog = {};
static CSVWriter motorControllerTemperatureLog = {};
static CSVWriter motorControllerVoltageLog = {};
static CSVWriter motorCurrentLog = {};
static CSVWriter rpmLog = {};
static CSVWriter thermistorLog = {};
static CSVWriter bmsVoltageLog = {};
static CSVWriter *logs[] = {&motorTemperatureLog, &motorControllerTemperatureLog, &motorControllerVoltageLog, &motorCurrentLog, &rpmLog, &thermistorLog, &bmsVoltageLog};

unsigned long timer = millis();
int cycleCount = 0;

int lowerUpperCells = -1;
unsigned long ms = millis();
byte sdStarted = 0;

SemaphoreHandle_t spi_mutex;

void initializeLogs()
{
  motorTemperatureLog = {MOTOR_TEMPERATURE_LOG, (uint8_t *)&motorTemp, 1, FLOAT};
  motorControllerTemperatureLog = {MOTOR_CONTROLLER_TEMPERATURE_LOG, (uint8_t *)&motorControllerTemp, 1, FLOAT};
  motorControllerVoltageLog = {MOTOR_CONTROLLER_VOLTAGE_LOG, (uint8_t *)&motorControllerBatteryVoltage, 1, FLOAT};
  motorCurrentLog = {MOTOR_CURRENT_LOG, (uint8_t *)&motorCurrent, 1, FLOAT};
  rpmLog = {RPM_LOG, (uint8_t *)&RPM, 1, FLOAT};
  thermistorLog = {THERMISTOR_LOG, (uint8_t *)&thTemps[0], 10, FLOAT};
  bmsVoltageLog = {BMS_VOLTAGE_LOG, (uint8_t *)&seriesVoltage, 1, FLOAT};
  dataLoggingTaskData = {logs, 7};
}

void initializeCANStructs()
{
  motorStats = {&RPM, &motorCurrent, &motorControllerBatteryVoltage, &errorMessage};
  motorTemps = {&throttle, &motorControllerTemp, &motorTemp, &controllerStatus};
  cellVoltages = {&cellVoltagesArr[0], &seriesVoltage, &cellsReady};
  bmsStatus = {&bms_status_flag, &bms_c_id, &bms_c_fault, &ltc_fault, &ltc_count};
  thermistorTemps = {thTemps};
  chargerStats = {&chargeFlag, &chargerStatusFlag, &chargerVoltage, &chargerCurrent, &chargerTemp};
  chargeControllerStats = {&evccEnable, &evccVoltage, &evccCurrent};
  canTaskData = {motorStats, motorTemps, bmsStatus, thermistorTemps, cellVoltages, chargerStats, chargeControllerStats, &seriesVoltage};
}

void initializePreChargeStruct()
{
  preChargeData = {bmsStatus, motorTemps, cellVoltages, &motorControllerBatteryVoltage};
}

void setup()
{
  pinMode(HIGH_VOLTAGE_TOGGLE, INPUT_PULLUP);
  pinMode(CLOSE_CONTACTOR_BUTTON, INPUT_PULLUP);
  pinMode(TFT_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);
  pinMode(TS_CS, OUTPUT);
  digitalWrite(TS_CS, HIGH);
  pinMode(PRECHARGE, OUTPUT);
  digitalWrite(PRECHARGE, LOW);
  pinMode(CONTACTOR, OUTPUT);
  digitalWrite(CONTACTOR, LOW);
  pinMode(CONTACTOR_PRECHARGED_LED, OUTPUT);
  digitalWrite(CONTACTOR_PRECHARGED_LED, LOW);
  pinMode(CONTACTOR_CLOSED_LED, OUTPUT);
  digitalWrite(CONTACTOR_CLOSED_LED, LOW);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, LOW);
  // motor temp points to motor controller temp for now
  measurementData = {&cellVoltages[0], &motorControllerBatteryVoltage, &auxiliaryBatteryVoltage, &RPM, &motorControllerTemp, &motorCurrent, &errorMessage,
                     , &chargerCurrent, &chargerVoltage, &bms_status_flag, &evccVoltage, thTemps};
  initializeCANStructs();
  // initial
  initializeLogs();

  Serial.print("Starting SD: ");
  if (startSD())
  {
    Serial.println("SD successfully started");
    sdStarted = 1;
  }
  else
  {
    sdStarted = 0;
    Serial.println("Error starting SD card");
  }
  setupDisplay(screen);
  setupCAN();
  initializePreChargeStruct();

  spi_mutex = xSemaphoreCreateMutex();

  portBASE_TYPE s1, s2, s3, s4, s5;
  s1 = xTaskCreate(prechargeTask, "PRECHARGE TASK", PRECHARGE_TASK_STACK_SIZE, (void *)&preChargeData, 5, NULL);
  s2 = xTaskCreate(canTask, "CAN TASK", CAN_TASK_STACK_SIZE, (void *)&canTaskData, 4, NULL);
  s3 = xTaskCreate(idleTask, "IDLE_TASK", configMINIMAL_STACK_SIZE, NULL, 1, NULL);
  s4 = xTaskCreate(displayTask, "DISPLAY TASK", DISPLAY_TASK_STACK_SIZE, (void *)&measurementData, 2, NULL);
  s5 = xTaskCreate(dataLoggingTask, "DATA LOGGING TASK", DATALOGGING_TASK_STACK_SIZE, (void *)&dataLoggingTaskData, 3, NULL);

  if (s1 != pdPASS || s2 != pdPASS || s3 != pdPASS || s4 != pdPASS || s5 != pdPASS)
  {
    Serial.println("Error creating tasks");
    while (1)
      ;
  }

  Serial.println("Starting the scheduler !");
  // start scheduler
  vTaskStartScheduler();
  // should never hit this point unless the scheduler fails
  Serial.println("Insufficient RAM");
}

void idleTask(void *taskData)
{
  while (1)
  {
    vTaskDelay((50 * configTICK_RATE_HZ) / 1000);
  }
}

bool get_SPI_control(unsigned int ms)
{
  return xSemaphoreTake(spi_mutex, ms);
}

void release_SPI_control(void)
{
  xSemaphoreGive(spi_mutex);
}

void loop()
{
}
