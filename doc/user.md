
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
    


角色管理
---------------------------------------------------------------------------
url:
		/roel/list
command:
		{
			"verbose":true,
			"count":10,
			"offset":0
		}		
查询系统中当前角色，参数verbose控制是否显示详细信息，count指定返回校色数量，offset为分页查询的起点；

----------------------------------------------------------------------------
url:
		/roel/get
command:
		{
			"rolename":"role1"
		}		
查询系统中指定角色名的角色；

----------------------------------------------------------------------------
url:
		/roel/modify
command:
		{
			"rolename":"role1",
			"textname":"",
			"textdescription":"",
			"acls":[
				{"acltype":"httpGet","topic":"/tsquery","priority":0,"allow":true},
				{"acltype":"httpPost","topic":"/roel/modify","priority":0,"allow":true}
			]
		}		
修改角色，参数rolename指定要修改的角色，textname和textdescription为可选项，acls设定校色的权限；

----------------------------------------------------------------------------