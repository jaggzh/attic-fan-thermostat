const char *ssid = "my-wifi-ssid";
const char *password = "mywifi-password";
IPAddress ip(192, 168, 1, 10);  // Static IP of this esp device
IPAddress gw(192, 168, 1, 1);   // Router/gw
IPAddress nm(255, 255, 255, 0); // Netmask
const char *update_user = "myaccount"; // HTTP auth user for OTA http update
const char *update_pw = "mypassword";  // HTTP auth password

