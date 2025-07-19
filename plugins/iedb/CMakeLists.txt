if (CJSON_FOUND AND WITH_TLS)
	add_definitions("-DWITH_CJSON")

	set(SQLITE3_INCLUDE_DIR "C:/test/sqlite")
	set(SQLITE3_LIBRARY "C:/test/sqlite/sqlite3.lib")
	set( CLIENT_INC ${mosquitto_SOURCE_DIR} ${mosquitto_SOURCE_DIR}/include
		${STDBOOL_H_PATH} ${STDINT_H_PATH} ${PTHREAD_INCLUDE_DIR}
		${OPENSSL_INCLUDE_DIR} ${mosquitto_SOURCE_DIR}/deps
		${mosquitto_SOURCE_DIR}/src ${SQLITE3_INCLUDE_DIR}
		${CJSON_INCLUDE_DIRS} )

	set( CLIENT_DIR ${mosquitto_BINARY_DIR}/lib ${CJSON_DIR})
	
	include_directories(${CLIENT_INC})
	link_directories(${CLIENT_DIR} ${mosquitto_SOURCE_DIR} )

	add_library(iedb MODULE
		db.c
		db.h
		util.c
		util.h
		rax.c
		rax.h
		rax_malloc.h
		tscompaction.c
		tscompaction.h
		tsrule.c
		tsrule.h
		device.c
		device.h
		auth.c
		dynamic_security.h
		roles.c
		rolelist.c
		acl.c
		groups.c
		grouplist.c
		clients.c
		clientlist.c
		mqtt.c
		tag.c
		sub_matches_sub.c
		json_help.c
		json_help.h
		plugin.c
		lzf_c.c
		lzf_d.c
		lzf.h
		lzfP.h
		md5.c
		md5.h
		sha256.c
		sha256.h)

	set_target_properties(iedb PROPERTIES
		POSITION_INDEPENDENT_CODE 1
	)
	set_target_properties(iedb PROPERTIES PREFIX "")

	target_link_libraries(iedb ${CJSON_LIBRARIES} ${OPENSSL_LIBRARIES} ${SQLITE3_LIBRARY})
	if(WIN32)
		target_link_libraries(iedb mosquitto)
		install(TARGETS iedb
			DESTINATION "${CMAKE_INSTALL_BINDIR}")
	else()
		install(TARGETS iedb
			RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
			LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
	endif()

endif()
