
1、把现在scripts目录下的js都放到example.md中。
2、创建一个testSetting.js。
- 用作先点击解锁按钮，然后在键盘上输入300416，再点确认键解锁。
- 打开设置程序，滑倒屏幕最下方
- 找到关于本机，点击
- 找到IMEI
- 在执行每次查询操作时，都通过console.log把执行的结果打印出来
- 最后执行锁定按钮

帮我把解锁和上锁合并到一个函数中，通过传参来区分。

=================================

帮我在Demo.js中新增一个开启和关闭VPN的函数，通过传参决定开启还是关闭。

开启关闭VPN需要打开的包是com.v2ray.ang。
等待com.v2ray.ang:id/tv_test_state出现。

连接的过程是
如果Text为"未连接"，
就等待500ms，点击按钮com.v2ray.ang:id/fab。
等待com.v2ray.ang:id/tv_test_state出现，并且Text内容包含已连接。
因为连接需要一个过程，点击按钮检查标签超时时间是20秒。
如果按钮中还有点击测试连接，则继续点击tv_test_state测试一下
检查内容出点 连接成功 则说明已经成功连接上。测试超时时间也是20秒。

如果打开应用后状态已经是已连接，你按照上面的方法测试下是不是连上了。

关闭的过程是
如果tv_test_state的标签不是未连接。
点击fab按钮，然后等tv_test_state出现未连接。
则说明已经关闭连接。

=========================



帮我在Demo.js中新增一个咨询gemini的函数
参数有
查询的内容。
相册名称
图片的序号
如果相册为空或者照片序号是-1则表示不上传图片。

打开的包是com.google.android.googlequicksearchbox

点击com.google.android.googlequicksearchbox:id/assistant_robin_input_collapsed_text_half_sheet，在其中输入查询的内容。

如果需要上传图片
点击
com.google.android.googlequicksearchbox:id/assistant_robin_chat_input_attachment_btn。
会弹出一个窗口
寻找其中Text为图库的TextView，点击
弹出选择框

继续寻找TextView为相册，点击。

然后寻找GridView为#picker_tab_recyclerview。

查询所有的Resource ID
com.google.android.providers.media.module:id/album_name。寻找对应相册的名字

如果没找到继续往下滑动。如果滑动后连续3次的album_name的数组都没变化，则说明滑倒底了，没有找到返回。
如果找到了，则点击。

寻找所有的#icon_check。根据传的序号，点击对应的按钮。
然后点击
com.google.android.providers.media.module:id/button_add。

等待10秒，等待照片完全上传完。
点击#assistant_robin_send_button，发送内容。
如果弹出#0_resource_name_obfuscated的TextView，里面包含评价会公开显示的评价。
点击Text为以后再的按钮。

等待60秒，等待程序的响应。

寻找RecyclerView #assistant_robin_chat_history_list。
寻找最后一条的#assistant_robin_text。拿到里面的Text。
作为返回。

帮我改下主逻辑，先解锁手机，打开vpn，然后去gemini，帮我问一下今天股价涨了没，然后关闭vpn，锁定手机。

======================

帮我实现一个打开应用的函数。
按下主屏幕按键，判断出现 android.view.View的Content Desc 主屏幕，说明出现了主屏幕。
网上话

帮我实现一个打开应用的函数。
打开包名为com.sonymobile.launcher/.XperiaLauncher
点击Resource ID com.android.launcher3:id/fallback_search_view的按钮。
输入要打开的应用名称。
寻找 androidx.recyclerview.widget.RecyclerView中
第一个android.widget.TextView，点击即可打开。


======

调整下script/route.js中的方法。
post用作获取日志。put用作执行脚本。
前端page.jsx在执行脚本时需要每秒调用日志的接口获取日志，并把日志追加到Log中展示给我。

src/appium.js中需要申请个全局变量用作存储日志，日志是个数组。

createAppiumWrapper中需要加个log函数和err函数，用作打印和存储日志，有新日志就把字符串存到数组中，如果数组长度超过1000条，就清空数组。如果传入的字符长度超过500，就只保留前500，后面用省略号表示，并在括号中注上原始长度。

appium.js中有个getLogs的静态函数，返回全局变量日志数组中的日志，取完要清空数组避免浪费内存。






