if (CJSON_FOUND AND WITH_TLS)
	add_definitions("-DWITH_CJSON")

	set( CLIENT_INC ${mosquitto_SOURCE_DIR} ${mosquitto_SOURCE_DIR}/include
		${STDBOOL_H_PATH} ${STDINT_H_PATH} ${PTHREAD_INCLUDE_DIR}
		${OPENSSL_INCLUDE_DIR} ${mosquitto_SOURCE_DIR}/deps
		${mosquitto_SOURCE_DIR}/src
		${CJSON_INCLUDE_DIRS} )

	set( CLIENT_DIR ${mosquitto_BINARY_DIR}/lib ${CJSON_DIR})

	include_directories(${CLIENT_INC})
	link_directories(${CLIENT_DIR} ${mosquitto_SOURCE_DIR})

	add_library(iedb MODULE
		device.c
		device.h
		auth.c
		json_help.c
		json_help.h
		plugin.c
		lzf_c.c
		lzf_d.c
		lzf.h
		lzfP.h
		sha256.c
		sha256.h)

	set_target_properties(iedb PROPERTIES
		POSITION_INDEPENDENT_CODE 1
	)
	set_target_properties(iedb PROPERTIES PREFIX "")

	target_link_libraries(iedb ${CJSON_LIBRARIES} ${OPENSSL_LIBRARIES})
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
