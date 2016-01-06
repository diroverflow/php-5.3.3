PVM
===
PVM是一个PHP脚本引擎，用来协助检测PHP一句话webshell。

本程序基于PHP 5.3.3修改而来，功能部分主要由两个模块组成：

一个是PHP扩展pwatch（ext/pwatch/），在webserver内监视关键函数调用（eval等），一旦有关键函数调用，则把GET/POST/COOKIE等请求数据发送给pvm模拟执行。

一个SAPI接口pvm（sapi/pvm/），作为主程序，负责接收GET/POST/COOKIE等请求数据，并调用Zend引擎执行，通过变量污点追踪的方式判断是否有外部数据流入关键函数（eval等）。
linux编译
---------
编译pwatch

cd ext/pwatch/

phpize

./configure

make

生成ext/pwatch/modules/pwatch.so

编译pvm

make

生成sapi/pvm/pvm


win编译
-------
require:

	VC 2008 SP1
	
	php-sdk-binary-tools-20110915.zip
	
	deps-5.3-vc9-x86.7z
	
step:

	unzip php-sdk-binary-tools-20110915.zip to php-sdk
	
	run bin\phpsdk_setvars.bat and bin\phpsdk_buildtree.bat phpdev in "Visual Studio 2008 Command Prompt"
	
	unzip deps-5.3-vc9-x86.7z to php-sdk\phpdev\vc9\x86\
	
	copy php-5.3.3 source code dir to php-sdk\phpdev\vc11\x86\php-5.5.3-src\
	
	run buildconf in php-sdk\phpdev\vc11\x86\php-5.5.3-src\
	
	configure --disable-zts
	
	nmake
	
	生成Release\pvm.exe
	
安装pwatch
-----------
编辑php.ini，加入pwatch扩展:extension=/path/to/pwatch.so
	
用法
-----
sapi/pvm/pvm [-g <GETSTRING>] [-p <POSTSTRING>] [-i <POSTDATAFILE>] [-k <COOKIESTRING>] [-t <METHODSTRING>] [-l <logfile>] [-f <file>]

  -d               decode POST data		编码POST数据，以\xAA\xBB的形式输入，避免不可见字符
  
  -g <querystring> GET data						QUERYSTRING数据
 
  -p <postdata>    POST data					POST数据
 
  -i <postdatafile>POST data file			POST数据文件

  -k <cookies>     COOKIE data				COOKIE数据

  -t <method>      METHOD(GET\POST)		请求方法，GET或POST

  -l <logfile>     log to file				把监视结果输出到文件

  -f <file>        Parse <file>				要执行的主php脚本

Release\pvm.exe -g c=d -p a=b -k e=f -t POST -f testscripts\11.php
	
显示出Warning: eval(): Eval code contains data that might be tainted in E:\PHP\php-5.3.3\testscripts\11.php on line 1
	
证明该php文件可能是一句话木马。