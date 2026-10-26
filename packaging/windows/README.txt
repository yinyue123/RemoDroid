RemoDroid Server @VERSION@ for Windows
=======================================

The RemoDroid control server: the web panel, logins, the STUN/TURN relay and HTTPS.
Windows Server 2019 or later, Windows 10 1809 or later, Windows 11.

Install
  1. Unzip this folder somewhere it can stay, e.g. C:\Program Files\RemoDroid Server.
  2. Run RemoDroid-Server.exe and choose "Install and start".
     It installs the "RemoDroid Server" Windows service, which starts with Windows
     whether or not anyone signs in, and lets it through Windows Firewall.
  3. In the same window, fill in the administrator, the web port, a domain for HTTPS
     (optional) and the STUN/TURN relay, then "Save and restart".
  4. "Open panel" opens the web panel.

The icon by the clock shows whether the server runs; its menu opens the settings, the log
and the panel, and starts, stops or restarts the server.

Cloud server (AWS, Azure, …): also open these ports in its firewall / security group:
  TCP  your web port (80), and 443 with a domain
  UDP  3478 and 49152-49252 (STUN/TURN)

Files
  Settings  %ProgramData%\RemoDroid\server.ini
  Log       %ProgramData%\RemoDroid\server.log
  (that folder is for administrators only)

Update: in RemoDroid-Server.exe press Stop, unzip the new version over this folder, press Start.

Without the app, from an administrator command prompt:
  remodroid-server install      install and start the service
  remodroid-server stop | start | restart | uninstall
  remodroid-server             run in this window instead, until Ctrl+C

https://github.com/yinyue123/RemoDroid
