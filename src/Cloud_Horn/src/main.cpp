#include <Arduino.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Update.h>
#include <Preferences.h>
#include <HTTPClient.h>

//功能描述
/*
//控制功放开关，功放音量，mqtt播报信息,输出音源 接有源音响
//支持有线联网和无线联网
//有线网卡和无线网卡 的mac地址
//提供电源+POE供电接口
//支持云端固件更新
//开发pc配置工具；提供对无线连接信息配置的方法（
*/

//程序当前版本
int Current_Version = 1;
//远程服务器版本
int OTA_Version;
// HTTP请求用的URL
#define URL "http://www.hongwans.net/Project/Cloud_Horn/version.txt"

//定于串口类 接TTS
HardwareSerial mySerial1(1);
//保存配置信息
Preferences prefs;
// TTS闲时状态 合成时高电平
#define TTS_BH_pin 19
//有线网卡网络状态
#define STA_pin 4
//音量调节 数字电位器
#define CS_pin 21
#define INC_pin 22
#define UD_pin 23
//功放开关
#define on_off_Pin 27
//指示灯
#define led_Pin 2

bool WiFi_state = false;
//无线网卡的MAC地址 本机地址
String network_mac;
//时间纪录
unsigned int timecnt;
//无线联网客户端
WiFiClient client;
// OTA 升级读取到服务区数据的长度
int contentLength = 0;
bool isValidContentType = false;

//在线升级
String host; // Host =>
int port;    // Non https. For HTTPS 443. As of today, HTTPS doesn't work.
String bin;  // bin file name with a slash in front.

//从报头中提取报头值
String getHeaderValue(String header, String headerName)
{
  return header.substring(strlen(headerName.c_str()));
}

// MQTT配置
PubSubClient mqttClient(client); //通过网络客户端连接创建mqtt连接客户端

//阿里云MQTT信息 未启用
/*ram登录账号： mqtt.aliyun.com@1875806428794132.onaliyun.com
密码： ldc}G0y#NbCWfv60JnCyDD}8fz)cECAM
实例id：post-cn-7mz2fgzjp08
公网接入地址：post-cn-7mz2fgzjp08.mqtt.aliyuncs.com
端口号：1883
accessKey：LTAI5tDorfc3Ntcic23AdH3A
secretKey：KxNBXet1JvNB6twXKmZI8im6Px6Hqc

accessKey：LTAI5tD1meCi9MMq3TX2srVN
secretKey：CmuHqQJJTN40ZAbISGErQwSw8JI5ys

订阅topic：all/MAC码（MAC码均使用有线网卡的MAC码）
qosLevel ： 0
clientId 规则：GID_TEST@@@ + MAC码（MAC码均使用有线网卡的MAC码）
*/

//--------------------

/*
发送topic：service
qosLevel：2

mqtt消息发送：

0.播放消息溢出提示
type = 0
mac：所有mac地址均使用有线网卡的mac地址
content：null

1.消息播放完毕提示
（所有消息播放完毕10s后发送该消息，如果10s内有新的消息进入，则先播放该消息，之后重新计算时间）
type = 1
mac：所有mac地址均使用有线网卡的mac地址
content：null
*/

// OTA Logic
void execOTA()
{
  Serial.println("Connecting to: " + String(host));
  //连接服务器
  if (client.connect(host.c_str(), port))
  {
    //创建 HTTPClient 对象
    HTTPClient httpClient;
    //配置请求地址。此处也可以不使用端口号和PATH而单纯的
    httpClient.begin(client, URL);
    //启动连接并发送HTTP请求
    int httpCode = httpClient.GET();
    Serial.print("Send GET request to URL: ");
    Serial.println(URL);

    //如果服务器响应OK则从服务器获取响应体信息并通过串口输出
    //如果服务器不响应OK则将服务器响应状态码通过串口输出
    if (httpCode == HTTP_CODE_OK)
    {
      String responsePayload = httpClient.getString();
      Serial.print("Server OTA_Version: ");
      Serial.println(responsePayload);
      OTA_Version = responsePayload.toInt();
    }
    else
    {
      Serial.println("Server Respose Code：");
      Serial.println(httpCode);
    }

    //关闭ESP与服务器连接
    httpClient.end();

    if (Current_Version != OTA_Version)
    {
      // 连接成功
      Serial.println("Fetching Bin: " + String(bin));

      // 获取bin文件
      client.print(String("GET ") + bin + " HTTP/1.1\r\n" +
                   "Host: " + host + "\r\n" +
                   "Cache-Control: no-cache\r\n" +
                   "Connection: close\r\n\r\n");

      // 调试用 检查请求的内容
      //    Serial.print(String("GET ") + bin + " HTTP/1.1\r\n" +
      //                 "Host: " + host + "\r\n" +
      //                 "Cache-Control: no-cache\r\n" +
      //                 "Connection: close\r\n\r\n");

      unsigned long timeout = millis();
      while (client.available() == 0)
      {
        if (millis() - timeout > 5000)
        {
          Serial.println("Client Timeout !");
          client.stop();
          return;
        }
      }

      while (client.available())
      {
        // 读取一行数据  /n
        String line = client.readStringUntil('\n');
        line.trim();

        if (!line.length())
        {
          break;
        }
        // 检查HTTP响应是否为200
        if (line.startsWith("HTTP/1.1"))
        {
          if (line.indexOf("200") < 0)
          {
            Serial.println("Got a non 200 status code from server. Exiting OTA Update.");
            break;
          }
        }

        //读取服务器返回的信息长度
        if (line.startsWith("Content-Length: "))
        {
          contentLength = atoi((getHeaderValue(line, "Content-Length: ")).c_str()); //读取到信息的长度
          Serial.println("Got " + String(contentLength) + " bytes from server");
        }

        //读取返回类型
        if (line.startsWith("Content-Type: "))
        {
          String contentType = getHeaderValue(line, "Content-Type: ");
          Serial.println("Got " + contentType + " payload.");
          if (contentType == "application/octet-stream") //字节流 下载
          {
            //确定类型为 下载
            isValidContentType = true;
          }
        }
      }

      //打印服务器返回的数据长度与类型
      Serial.println("contentLength : " + String(contentLength) + ", isValidContentType : " + String(isValidContentType));

      if (contentLength && isValidContentType)
      {
        //检查OTA更新需要的空间
        bool canBegin = Update.begin(contentLength);
        if (canBegin)
        {
          Serial.println("在线升级开始。可能需要2-5分钟来完成。请耐心等待!");
          size_t written = Update.writeStream(client);

          if (written == contentLength)
          {
            //数据读取成功
            Serial.println("Written : " + String(written) + " successfully");
          }
          else
          {
            Serial.println("Written only : " + String(written) + "/" + String(contentLength) + " failure");
          }

          if (Update.end())
          {
            Serial.println("OTA done!");
            if (Update.isFinished())
            {
              Serial.println("Update successfully completed. Rebooting.");
              ESP.restart();
            }
            else
            {
              Serial.println("Update not finished? Something went wrong!");
            }
          }
          else
          {
            Serial.println("Error Occurred. Error #: " + String(Update.getError()));
          }
        }
        else
        {
          // OAT升级空间不足
          Serial.println("Not enough space to begin OTA");
          client.flush();
        }
      }
    }
    else
    {
      Serial.println("OTA 没有更新!");
      client.flush();
    }
  }
  else
  {
    Serial.println("There was no content in the response");
    client.flush();
  }
}
//网络状态 准备就绪 高电平
int GetWired_network_STA()
{
  return digitalRead(STA_pin);
}

// TTS闲时状态 合成时高电平
int GetTTS_STA()
{
  return digitalRead(TTS_BH_pin);
}

// tts 语音合成
void TTS(char str[])
{
  mySerial1.write(str);
}

//功放开
void PowerAmplifier_ON()
{
  //功放开关
  digitalWrite(on_off_Pin, LOW);
}

//功放关
void PowerAmplifier_OFF()
{
  //功放开关
  digitalWrite(on_off_Pin, HIGH);
}

//调节音量 1增加 0减少  num次数
void Wiper_Test(int UD, int num)
{
  digitalWrite(CS_pin, LOW); // CS置低，使用这块芯片
  delay(1);
  if (UD == 1)
  {
    digitalWrite(UD_pin, HIGH); //如果你选择增加输出电压则将UD引脚置高
  }
  else
  {
    digitalWrite(UD_pin, LOW); //反之，置低
  }
  delay(1);
  for (; num > 0; num--) //这一段是控制调节几次电阻的，如果想要调节多一点就使INC多经历几次下降沿，每次调节1010欧姆
  {
    digitalWrite(INC_pin, LOW);

    delay(1);
    digitalWrite(INC_pin, HIGH);
    delay(1);
  }
  digitalWrite(CS_pin, HIGH); //将CS置高产生一个上升沿，INC在执行完上面一段后也被置高，因此固定到了当前的阻值
  delay(20);                  //芯片手册上说不小于20ms，应该是挂载多个设备的时候使用的。
}

//音量初始化 先把音量调到最低 然后升高到 5 并记录
void Wiper_info(int num)
{
  //调节音量 1增加 0减少  num次数
  Wiper_Test(0, 100);
  //调节音量 1增加 0减少  num次数
  Wiper_Test(1, num);
}

//无线网络初始化
void WiFi_info(String s_SSID, String s_PSWD)
{
  const char *SSID = s_SSID.c_str();
  const char *PSWD = s_PSWD.c_str();

  Serial.println("Connecting to " + String(SSID));
  WiFi.begin(SSID, PSWD);

  //************************************************************************
  //需要处理 网络连接超时  问题
  //************************************************************************
  // Wait for connection to establish
  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.print("."); // Keep the serial monitor lit!
    delay(500);
    if (millis() - timecnt > (1000 * 10))
    {
      Serial.println("无线网络连接超时");
      break;
    }
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    // 连接成功
    Serial.println("");
    Serial.println("Connected to " + String(SSID));
    WiFi_state = true;
    // 检查 OTA 升级
    execOTA();
  }
  else
  {
    WiFi_state = false;
  }
}

//处理MQTT收到的信息 并做出反馈
void DealWith(int type, String content)
{
  switch (type)
  {
    /*
    0.播放传入信息：
    type = 0
    content：这是需要播放的信息
    */
  case 0:
  {
    int str_len = content.length() + 1;
    char char_array[str_len];
    int commaPosition;
    int index = 0;
    do
    {
      commaPosition = content.indexOf(',');
      if (commaPosition != -1)
      {
        //调试打印源内容
        // Serial.println(content.substring(0 + 2, commaPosition));
        long result = strtol((const char *)content.substring(0 + 2, commaPosition).c_str(), 0, 16);
        char_array[index] = result;
        index++;
        content = content.substring(commaPosition + 1, content.length());
      }
      else
      {
        if (content.length() > 0)
        {
          byte result = strtol((const char *)content.substring(0 + 2, commaPosition).c_str(), 0, 16);
          char_array[index] = result;
        }
      }
    } while (commaPosition >= 0);
    TTS(char_array);
    break;
  }
  /*
   1.打开喇叭：
    type = 1
    content: null
  */
  case 1:
    PowerAmplifier_ON();
    break;

    /*
     2.关闭喇叭：
    type = 2
    content:null
    */
  case 2:
    PowerAmplifier_OFF();
    break;
    /*
    3.改变音量
    （喇叭的初始音量默认设置为50）
    type = 3
    content : 需要改变成的值，为百分数（如传入60则音量为60%）
    */
  case 3:
    //内置8种音效，编号为0-7发送”<Z>”+编号控制播放内置音效，代码如：printf(“<Z>0”);播报编号为0的音效
    //发送”<V>”+音量等级设置音量播报，可设置1-4级音量，代码如：printf(“<V>3”);设置音量为3。系统默认为4，为最高音量。
    //发送”<S>”+语速值设置语速，可设置1-3级语速，代码如：printf(“<S>3”);设置语速为3。系统默认为2，为中速。
    //发送”<I>1”开启上电音效提示，”<I>0”则关闭上电音效提示。系统默认开启。
    TTS((char *)content.c_str());
    // iper_info(1);
    break;
    /*
    4.停止播放
    type = 4
    content : null
    */
  case 4:
    char strd[] = {0xB2, 0xA5, 0x20, 0xB7, 0xC5, 0x20, 0xCD, 0xEA, 0x20, 0xB1, 0xCF, 0x20, 0xA3, 0xA1};
    TTS(strd);
    break;
  }
}
//收到set主题的命令下发时的回调函数,(接收命令)
void callback(char *topic, byte *payload, unsigned int length)
{
  // Serial.println("收到下发的命令主题:");
  // Serial.println(topic);
  // Serial.println("下发的内容是:");
  // payload[length] = '\0'; //为payload添加一个结束附,防止Serial.println()读过了
  // Serial.println((char *)payload);

  //接下来是收到的json字符串的解析
  DynamicJsonDocument doc(100);
  DeserializationError error = deserializeJson(doc, payload);
  if (error)
  {
    Serial.println("parse json failed");
    return;
  }
  JsonObject setAlinkMsgObj = doc.as<JsonObject>();
  serializeJsonPretty(setAlinkMsgObj, Serial);
  //调试信息 打印收到的源文
  Serial.println();

  //处理收到的信息
  int type = setAlinkMsgObj["type"];
  String content = setAlinkMsgObj["content"];
  DealWith(type, content);
}

// MQTT初始化
void Mqtt_info(String s_mqttServer, int mqttPort, String s_id, String s_topic, String s_mqttUser, String s_mqttPassword)
{

  const char *mqttServer = s_mqttServer.c_str();
  const char *id = s_id.c_str();
  const char *topic = s_topic.c_str();
  const char *mqttUser = s_mqttUser.c_str();
  const char *mqttPassword = s_mqttPassword.c_str();

  mqttClient.setServer(mqttServer, mqttPort);
  while (!client.connected())
  {
    Serial.println("Connectingto MQTT...");
    if (mqttClient.connect(id, mqttUser, mqttPassword))
    {
      Serial.println("connected");
    }
    else
    {
      Serial.print("failedwith state ");
      Serial.println(mqttClient.state());
      delay(2000);
    }
  }
  //订阅主题
  mqttClient.subscribe(topic);
  //发布主题消息
  mqttClient.publish("service", "Hello");
  //接收消息回调
  mqttClient.setCallback(callback); //绑定收到set主题时的回调(命令下发回调)
}
//程序入口
void setup()
{
  Serial.begin(115200);                     //调试接口 接pc工具
  mySerial1.begin(9600, SERIAL_8N1, 5, 18); // tts 需要发送GBK编码
  Serial2.begin(115200);                    //有线网卡

  //记录时间初始化
  timecnt = millis();

  //无线网络相关
  WiFi.mode(WIFI_MODE_STA);
  Serial.print("ESP32 Board MAC Address:  ");
  network_mac = WiFi.macAddress();
  Serial.println(network_mac);

  // TTS闲时状态 合成时高电平
  pinMode(TTS_BH_pin, INPUT_PULLUP);
  //网络状态
  pinMode(STA_pin, INPUT_PULLUP);
  //音量调节 数字电位器
  pinMode(CS_pin, OUTPUT);
  pinMode(INC_pin, OUTPUT);
  pinMode(UD_pin, OUTPUT);
  //初始化
  digitalWrite(CS_pin, LOW);
  digitalWrite(INC_pin, LOW);
  digitalWrite(UD_pin, LOW);
  //功放开关
  pinMode(on_off_Pin, OUTPUT);
  PowerAmplifier_OFF();
  //指示灯
  pinMode(led_Pin, OUTPUT);
  //亮灯
  digitalWrite(led_Pin, HIGH);
  //音量初始化
  // Wiper_info(1);

  //配置信息
  prefs.begin("mynamespace");                 //打开命名空间mynamespace
  uint32_t count = prefs.getUInt("count", 0); // 获取当前命名空间中的键名为"count"的值 如果没有该元素则返回默认值0
  count++;                                    // 累加计数
  Serial.printf("升级后 这是系统第 %u 次启动\n", count);
  prefs.putUInt("count", count); // 将数据保存到当前命名空间的"count"键中

  String p_Mac = prefs.getString("Mac", "");
  if (p_Mac == "")
  {
    //配置信息初始化
    prefs.putString("Mac", network_mac);                        // 保存数据
    prefs.putString("MqttServer", "47.112.134.233");            // 保存数据
    prefs.putString("MqttPort", "1883");                        // 保存数据
    prefs.putString("MqttUser", "test");                        // 保存数据
    prefs.putString("MqttPassword", "test");                    // 保存数据
    prefs.putString("Id", "GID_" + network_mac);                // 保存数据
    prefs.putString("Topic", "all/" + network_mac);             // 保存数据
    prefs.putString("SSID", "CMCC-艾斯维尔");                   // 保存数据
    prefs.putString("PSWD", "www.YouMayDraw.com");              // 保存数据
    prefs.putString("Host", "www.hongwans.net");                // 保存数据
    prefs.putString("Port", "80");                              // 保存数据
    prefs.putString("Bin", "/Project/Cloud_Horn/firmware.bin"); // 保存数据
  }

  //获取无线网络信息
  String s_SSID = prefs.getString("SSID", "");
  String s_PSWD = prefs.getString("PSWD", "");

  //在线升级
  host = prefs.getString("Host", "");
  port = prefs.getString("Port", "80").toInt();
  bin = prefs.getString("Bin", "");

  //MQTT初始化
  String s_mqttServer = prefs.getString("MqttServer", "");
  int mqttPort = prefs.getString("MqttPort", "1883").toInt();
  String s_mqttUser = prefs.getString("MqttUser", "");
  String s_mqttPassword = prefs.getString("MqttPassword", "");
  String s_id = prefs.getString("Id", "");
  String s_topic = prefs.getString("Topic", "");

  prefs.end(); // 关闭当前命名空间

  if (count > 500)
  {
    Serial.println("系统已过试用期");
    delay(5000);
    ESP.restart(); // 重启系统
  }

  //网络初始化
  WiFi_info(s_SSID, s_PSWD);
  if (WiFi_state)
  {
    // MQTT初始化
    Mqtt_info(s_mqttServer, mqttPort, s_id, s_topic, s_mqttUser, s_mqttPassword);
  }
}

//程序执行
void loop()
{
  if (!mqttClient.connected() && WiFi_state) //如果客户端没连接ONENET, 重新连接
  {
    //配置信息
    prefs.begin("mynamespace"); //打开命名空间mynamespace
    //MQTT
    String s_mqttServer = prefs.getString("MqttServer", "");
    int mqttPort = prefs.getString("MqttPort", "1883").toInt();
    String s_mqttUser = prefs.getString("MqttUser", "");
    String s_mqttPassword = prefs.getString("MqttPassword", "");
    String s_id = prefs.getString("Id", "");
    String s_topic = prefs.getString("Topic", "");
    prefs.end(); // 关闭当前命名空间

    // MQTT初始化
    Mqtt_info(s_mqttServer, mqttPort, s_id, s_topic, s_mqttUser, s_mqttPassword);
    delay(100);
  }
  if (WiFi_state)
  {
    mqttClient.loop(); //客户端循环检测
  }
  //读取PC串口指令
  String pc_com_data = "";
  while (Serial.available() > 0) //判断串口是否接收到数据
  {
    pc_com_data += char(Serial.read()); //获取串口接收到的数据
    delay(2);
  }
  pc_com_data.trim();
  if (pc_com_data != "")
  {
    Serial.println(pc_com_data);
    //接下来是收到的json字符串的解析
    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, pc_com_data);
    if (error)
    {
      Serial.println("parse json failed");
      return;
    }
    JsonObject setAlinkMsgObj = doc.as<JsonObject>();
    serializeJsonPretty(setAlinkMsgObj, Serial);
    //调试信息 打印收到的源文
    Serial.println();

    //处理收到的信息
    int pc_type = setAlinkMsgObj["Type"];
    String pc_MqttServer = setAlinkMsgObj["MqttServer"];
    String pc_MqttPort = setAlinkMsgObj["MqttPort"];
    String pc_MqttUser = setAlinkMsgObj["MqttUser"];
    String pc_MqttPassword = setAlinkMsgObj["MqttPassword"];
    String pc_SSID = setAlinkMsgObj["SSID"];
    String pc_PSWD = setAlinkMsgObj["PSWD"];
    String pc_Host = setAlinkMsgObj["Host"];
    String pc_Port = setAlinkMsgObj["Port"];

    //配置信息
    prefs.begin("mynamespace"); //打开命名空间mynamespace
    String Mac = prefs.getString("Mac", "");
    //获取无线网络信息
    String s_SSID = prefs.getString("SSID", "");
    String s_PSWD = prefs.getString("PSWD", "");
    //在线升级
    String host = prefs.getString("Host", "");
    int port = prefs.getString("Port", "80").toInt();
    String bin = prefs.getString("Bin", "");
    //MQTT
    String s_mqttServer = prefs.getString("MqttServer", "");
    int mqttPort = prefs.getString("MqttPort", "1883").toInt();
    String s_mqttUser = prefs.getString("MqttUser", "");
    String s_mqttPassword = prefs.getString("MqttPassword", "");
    prefs.end(); // 关闭当前命名空间

    String sendData = "{\"Mac\":\"" + Mac + "\",\"MqttServer\":\"" + s_mqttServer + "\",\"MqttPort\":\"" + mqttPort + "\",\"MqttUser\":\"" + s_mqttUser + "\",\"MqttPassword\":\"" + s_mqttPassword + "\",\"SSID\":\"" + s_SSID + "\",\"PSWD\":\"" + s_PSWD + "\",\"Host\":\"" + host + "\",\"Port\":\"" + port + "\",\"Bin\":\"" + bin + "\"}";
    switch (pc_type)
    {
    case 0:
      delay(200);
      Serial.println(sendData);
      delay(2);
      break;

    case 1:
      ESP.restart();
      break;

    case 3:
      //保存配置信息 初始化
      prefs.begin("mynamespace");                       //打开命名空间mynamespace
      prefs.putString("MqttServer", pc_MqttServer);     // 保存数据
      prefs.putString("MqttPort", pc_MqttPort);         // 保存数据
      prefs.putString("MqttUser", pc_MqttUser);         // 保存数据
      prefs.putString("MqttPassword", pc_MqttPassword); // 保存数据
      prefs.putString("SSID", pc_SSID);                 // 保存数据
      prefs.putString("PSWD", pc_PSWD);                 // 保存数据
      prefs.putString("Host", pc_Host);                 // 保存数据
      prefs.putString("Port", pc_Port);                 // 保存数据
      prefs.end();                                      // 关闭当前命名空间
      delay(2);
      ESP.restart();
      break;

    default:
      break;
    }
  }

  //-----------------------------------
  //读取有线网卡指令
  String Wired_com_data = "";
  while (Serial2.available() > 0) //判断串口是否接收到数据
  {
    Wired_com_data += char(Serial2.read()); //获取串口接收到的数据
    delay(2);
  }

  Wired_com_data.trim();
  if (Wired_com_data != "")
  {
    //接下来是收到的json字符串的解析
    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, pc_com_data);
    if (error)
    {
      Serial.println("parse json failed");
      return;
    }
    JsonObject setAlinkMsgObj = doc.as<JsonObject>();
    serializeJsonPretty(setAlinkMsgObj, Serial);
    //调试信息 打印收到的源文
    Serial.println();

    //处理收到的信息
    int type = setAlinkMsgObj["type"];
    String content = setAlinkMsgObj["content"];
    //没有无线网络的时候使用
    if (!WiFi_state)
    {
      DealWith(type, content);
    }
  }
}