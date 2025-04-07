
用户管理
------------------

url：
    /user/createClient
body:    
    {
    	"username":"u123",
    	"clientid":"c123",
    	"textname":"",
    	"textdescription":"",
    	"roles":[
    		{"rolename":"role1","priority":1},
    		{"rolename":"role2","priority":10}
    	],
    	"groups":[
    		{"groupname":"group1","priority":1},
    		{"groupname":"group2","priority":2}
    	]
    }
    
逆序查询采样数据
------------------

命令格式：
    TSREVQUERY end start [COUNT <n>] [FILTER_FIRST_NONE]  d1 f1 f2 [DEVICE d2] [DEVICE d3 f4 f5 f6]
    
    start end为起止时间戳，单位秒
    COUNT是可选的，用于设定指定数目的采样数据，FIELDS也是可选的，如果不设定，查询所有字段；
    FILTER_FIRST_NONE,是可选的，如果指定，则会过滤第一行值为none的记录
返回内容：

		1) 1) 1526985692746 
			 2) 1) 1) "Tempature"
						 2) "56.2"
        	2) 1) "pressure"
        		 2) "4.3"  
		2) 1) 1526985691746 
			 2) 1) 1) "Tempature"
						 2) "56.6"
        	2) 1) "pressure"
        		 2) "7.3"      
    
示例：    

		TSREVQUERY device1 1699268456700  1699268400000 
		TSREVQUERY device1 1699268456700 1699268400000 COUNT 100
		TSREVQUERY device1 1699268456700 1699268400000 FIELDS field1 field2
		TSREVQUERY device1 1699268456700 1699268400000 COUNT 100 FIELDS field1 field2
		
		
聚合查询采样数据
------------------

命令格式：
    TSAGGQUERY start end [COUNT <n>] d1 MAX(field1,60,60) MIN(field2,60,60) [DEVICE d2 MAX(field3,60,60)]
    
    start end为起止时间戳，单位秒
    COUNT是可选的，用于设定指定数目的采样数据，指定聚合函数，聚合函数第一个参数为字段名称，第二个为聚合窗口大小，单位为秒，第三个是聚合周期，单位为秒，必须为大于60秒，且小于等于聚合窗口，支持的集合函数为：MIN、MAX、SUM、AVG、TWA、STDP、STDS、VARP、VARS、COUNT、FIRST、LAST、RANGE；
    
返回内容：

		1) 1) 1526985691746 
			 2) 1) 1) "MAX(field1,100000)"
						 2) "56.2"
        	2) 1) "MIN(field2,20000)"
        		 2) "4.3"  
		2) 1) 1526985691746 
			 2) 1) 1) "MAX(field1,100000)"
						 2) "56.6"
        	2) 1) "MIN(field2,20000)"
        		 2) "7.3"      
    
示例：    

		TSAGGQUERY device1 1699268400000 1699268456700 FIELDS MAX(field1,600000,60000) MIN(field2,600000,60000)
		TSAGGQUERY device1 1699268400000 1699268456700 COUNT 100 FIELDS MAX(field1,600000,60000) MIN(field2,600000,60000)


逆序聚合查询采样数据
------------------

命令格式：
    TSREVAGGQUERY end start [COUNT <n>]  d1 MAX(field1,60,60) MIN(field2,60,60) [DEVICE d2 MAX(field3,60,60)]
    
    start end为起止时间戳，单位秒，
    COUNT是可选的，用于设定指定数目的采样数据，指定聚合函数，聚合函数第一个参数为字段名称，第二个为聚合窗口大小，单位为秒，第三个是聚合周期，单位为秒，必须为大于60秒，且小于等于聚合窗口，支持的集合函数为：MIN、MAX、SUM、AVG、TWA、STDP、STDS、VARP、VARS、COUNT、FIRST、LAST、RANGE；
    
返回内容：

		1) 1) 1526985691746 
			 2) 1) 1) "MAX(field1,100000)"
						 2) "56.2"
        	2) 1) "MIN(field2,20000)"
        		 2) "4.3"  
		2) 1) 1526985691746 
			 2) 1) 1) "MAX(field1,100000)"
						 2) "56.6"
        	2) 1) "MIN(field2,20000)"
        		 2) "7.3"      
    
示例：    

		TSREVAGGQUERY device1 1699268456700 1699268400000 FIELDS MAX(field1,600000,60000) MIN(field2,600000,60000)
		TSREVAGGQUERY device1 1699268456700 1699268400000 COUNT 100 FIELDS MAX(field1,600000,60000) MIN(field2,600000,60000)


首尾查询采样数据
------------------

命令格式：
    TSFLQUERY start end d1 f1 f2 [DEVICE d2] [DEVICE d3 f4 f5 f6]
    
    start end为起止时间戳，单位秒，
    DEVICE 是可选的，如果有则可查询更多设备，设备名后字段名列表，如果没有字段列表，则查询所有字段；
    
返回内容：

		1) "d1" 
		2) 1) "f11"
			2) 1) 1) "Timestamp"
        	  2) 1726197509438
        	  3) "Value" 
        	  4)10 
         2) 1) "Timestamp"
        	  2) 1726709178948
        	  3) "Value" 
        	  4)434   
        	  
		  3) "f12"
			4) 1) 1) "Timestamp"
        	  2) 1726197509438
        	  3) "Value" 
        	  4)10 
         2) 1) "Timestamp"
        	  2) 1726709178948
        	  3) "Value" 
        	  4)434           	  
    
示例：    

		TSFLQUERY 1699268456700 1699268400000 d1 f11 f12
		TSFLQUERY 1699268456700 1699268400000 d1 f11 f12 DEVICE d2 f20 f21 DEVICE d3 f30 f31 f33



累积量查询采样数据
------------------

命令格式：
    TSDIFFQUERY period start end d1 f1 f2 [DEVICE d2] [DEVICE d3 f4 f5 f6]
    
    period是周期，单位分钟，start end为起止时间戳，单位秒，
    DEVICE 是可选的，如果有则可查询更多设备，设备名后字段名列表，如果没有字段列表，则查询所有字段；
          	     
示例：    

		TSDIFFQUERY 10 1699268456700 1699268400000 d1 f11 f12
		TSDIFFQUERY 10 1699268456700 1699268400000 d1 f11 f12 DEVICE d2 f20 f21 DEVICE d3 f30 f31 f33

	
查询当前实时值
------------------

命令格式：
    TSLASTQUERY d1 f1 f2 [DEVICE d2] [DEVICE d3 f4 f5 f6]
    
    DEVICE 是可选的，如果有则可查询更多设备，设备名后字段名列表，如果没有字段列表，则查询所有字段；
          	     
示例：    

		TSLASTQUERY d1 f11 f12
		TSLASTQUERY d1 f11 f12 DEVICE d2 f20 f21 DEVICE d3 f30 f31 f33	
	
		
查询当前实时值（模式匹配）
------------------

命令格式：
    TSSCAN device_pattern field_pattern device_cursor field_cursor count
    
    device_pattern为搜索device名称模式，支持的格式如下：
    
    h?llo 匹配 hello, hallo 和 hxllo
		h*llo 匹配 hllo 和 heeeello
		h[ae]llo 匹配 hello 和 hallo, 但不匹配 hillo
		h[^e]llo 匹配 hallo, hbllo, ... 但不匹配 hello
		h[a-b]llo 匹配 hallo 和 hbllo
		
		field_pattern为搜索field名称模式，支持的格式类似device_pattern
		
		device_cursor为设备遍历的光标，初始值为0，field_cursor设备内字段遍历光标，初始值为0，下一次的TSSCAN的调用传入上一次返回的设备光标和字段光标，当两个光标都返回0，迭代结束；
		count为一次调用返回的最大字段数目；
    
成功返回内容：

		1) 1) 1)device1 
			    2) 1)	1)field1
			 		 		  2)10
			 	     2)	1)field2
			 	  		  2)30
		   2) 1)device2 
			 		2) 1)	1)field1
			 		 			2)10
			 	  	 2)	1)field2
			 	  		  2)30
		2) "5"    //设备光标
		3) "8"    //字段光标 
       
示例：    

		TSSCAN device* 0 0 10


查询所有设备字段名称
------------------

命令格式：
    ALLDEVICESFIELDS


查询所有设备名称
------------------

命令格式：
    ALLDEVICES


查询某个设备下所有字段
------------------

命令格式：
    ALLFIELDS device_name
    


新增设备
------------------

命令格式：
    NEWDEVICE device_name



BOOL数据聚合查询
------------------

命令格式：
    TSBOOLQUERY start end d1 f1 f2 [DEVICE d2] [DEVICE d3 f4 f5 f6]
    
    start end为起止时间戳，单位秒，
    DEVICE 是可选的，如果有则可查询更多设备，设备名后字段名列表，如果没有字段列表，则查询所有BOOL类型字段；    
    
    
上传时序数据文件
------------------

命令格式：
    UPLOAD/TSDATA/device_name 
    
    ts,f1,f2,f3
		1741661500,true,8,kk
		1741661600,false,9,hg3
		
    第一行为字段名称，第一列为时间戳，其他列为字段值；        