INCLUDE_DIRECTORIES (
    ${STAR_EXTERN_INCLUDES}
    ${STAR_CORE_INCLUDES}
    ${STAR_PLATFORM_INCLUDES}
    ${STAR_APPLICATION_INCLUDES}
  )

SET (star_application_HEADERS
    StarApplication.hpp
    StarApplicationController.hpp
    StarMainApplication.hpp
    StarRenderer.hpp
  )

SET (star_application_SOURCES
    StarApplication.cpp
    StarRenderer.cpp
  )

SET (star_application_HEADERS ${star_application_HEADERS}
    StarP2PNetworkingService_pc.hpp
    StarPlatformServices_pc.hpp
    StarPlatformServices_stub.hpp
    StarRenderer_opengl.hpp
  )

SET (star_application_SOURCES ${star_application_SOURCES}
    StarMainApplication_sdl.cpp
    StarP2PNetworkingService_pc.cpp
    StarPlatformServices_pc.cpp
    StarPlatformServices_stub.cpp
    StarRenderer_opengl.cpp
  )

IF (STAR_PLATFORM_N3DS)
  # Nintendo 3DS: replace desktop SDL/OpenGL/PC-platform files with N3DS stubs.
  # StarPlatformServices_stub is already included above and kept for N3DS too.
  LIST (REMOVE_ITEM star_application_HEADERS
      StarP2PNetworkingService_pc.hpp
      StarPlatformServices_pc.hpp
      StarRenderer_opengl.hpp
  )
  LIST (REMOVE_ITEM star_application_SOURCES
      StarMainApplication_sdl.cpp
      StarP2PNetworkingService_pc.cpp
      StarPlatformServices_pc.cpp
      StarRenderer_opengl.cpp
  )
  LIST (APPEND star_application_HEADERS
      StarRenderer_n3ds_stub.hpp
  )
  LIST (APPEND star_application_SOURCES
      StarMainApplication_n3ds_stub.cpp
      StarN3dsCitraCompat.cpp
      StarRenderer_n3ds_stub.cpp
  )
ENDIF ()

IF (STAR_ENABLE_STEAM_INTEGRATION)
  SET (star_application_HEADERS ${star_application_HEADERS}
      StarDesktopService_pc_steam.hpp
      StarStatisticsService_pc_steam.hpp
      StarUserGeneratedContentService_pc_steam.hpp
    )
  SET (star_application_SOURCES ${star_application_SOURCES}
      StarDesktopService_pc_steam.cpp
      StarStatisticsService_pc_steam.cpp
      StarUserGeneratedContentService_pc_steam.cpp
    )
ENDIF ()

IF (STAR_ENABLE_DISCORD_INTEGRATION)
  SET (star_application_SOURCES ${star_application_SOURCES}
      discord/core.cpp
      discord/image_manager.cpp
      discord/lobby_manager.cpp
      discord/network_manager.cpp
      discord/overlay_manager.cpp
      discord/relationship_manager.cpp
      discord/storage_manager.cpp
      discord/store_manager.cpp
      discord/types.cpp
      discord/user_manager.cpp
      discord/voice_manager.cpp
      discord/achievement_manager.cpp
      discord/activity_manager.cpp
      discord/application_manager.cpp
    )
ENDIF ()

ADD_LIBRARY (star_application OBJECT ${star_application_SOURCES} ${star_application_HEADERS})

IF(STAR_PRECOMPILED_HEADERS)
  TARGET_PRECOMPILE_HEADERS (star_application REUSE_FROM star_core)
ENDIF()