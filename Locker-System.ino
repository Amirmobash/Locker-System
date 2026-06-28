#include <ThreeWire.h>
#include <RtcDS1302.h>

#define RTC_CLK D1
#define RTC_DAT D2
#define RTC_RST D0

ThreeWire myWire(RTC_DAT, RTC_CLK, RTC_RST);
RtcDS1302<ThreeWire> Rtc(myWire);

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("DS1302 TEST");

  Rtc.Begin();
  Rtc.SetIsWriteProtected(false);
  Rtc.SetIsRunning(true);
  Rtc.SetDateTime(RtcDateTime(2026, 6, 25, 19, 47, 0));
}

void loop() {
  RtcDateTime now = Rtc.GetDateTime();

  Serial.print("Valid: ");
  Serial.print(now.IsValid());
  Serial.print("  ");
  Serial.print(now.Day());
  Serial.print(".");
  Serial.print(now.Month());
  Serial.print(".");
  Serial.print(now.Year());
  Serial.print(" ");
  Serial.print(now.Hour());
  Serial.print(":");
  Serial.print(now.Minute());
  Serial.print(":");
  Serial.println(now.Second());

  delay(1000);
}
