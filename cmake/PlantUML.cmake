#[[
  Renders the PlantUML sources in docs/plantuml/ to PNG + SVG.

  PlantUML is a java tool; if neither a "plantuml" launcher nor a
  plantuml.jar + java are available we skip the target.  The .puml sources are
  the real deliverable and are readable as text either way.

      cmake --build build --target diagrams
]]

find_program(MOVIEBACKEND_PLANTUML_EXECUTABLE plantuml)
find_program(MOVIEBACKEND_JAVA_EXECUTABLE java)

# CONFIGURE_DEPENDS makes the build re-run this glob when the directory
# contents change. Without it, a .puml file added after the last configure is
# silently ignored and the target fails with "No diagram found".
file(GLOB MOVIEBACKEND_PUML_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/docs/plantuml/*.puml")

set(MOVIEBACKEND_DIAGRAM_OUTPUT "${CMAKE_BINARY_DIR}/docs/diagrams")

if(MOVIEBACKEND_PLANTUML_EXECUTABLE)
    add_custom_target(diagrams
        COMMAND ${CMAKE_COMMAND} -E make_directory "${MOVIEBACKEND_DIAGRAM_OUTPUT}"
        COMMAND ${MOVIEBACKEND_PLANTUML_EXECUTABLE}
                -tsvg -o "${MOVIEBACKEND_DIAGRAM_OUTPUT}" ${MOVIEBACKEND_PUML_SOURCES}
        COMMAND ${MOVIEBACKEND_PLANTUML_EXECUTABLE}
                -tpng -o "${MOVIEBACKEND_DIAGRAM_OUTPUT}" ${MOVIEBACKEND_PUML_SOURCES}
        COMMENT "Rendering PlantUML diagrams -> ${MOVIEBACKEND_DIAGRAM_OUTPUT}"
        VERBATIM)
    message(STATUS "MovieBackend: 'diagrams' target available (plantuml)")
elseif(MOVIEBACKEND_JAVA_EXECUTABLE AND EXISTS "$ENV{PLANTUML_JAR}")
    add_custom_target(diagrams
        COMMAND ${CMAKE_COMMAND} -E make_directory "${MOVIEBACKEND_DIAGRAM_OUTPUT}"
        COMMAND ${MOVIEBACKEND_JAVA_EXECUTABLE} -jar "$ENV{PLANTUML_JAR}"
                -tsvg -o "${MOVIEBACKEND_DIAGRAM_OUTPUT}" ${MOVIEBACKEND_PUML_SOURCES}
        COMMENT "Rendering PlantUML diagrams via plantuml.jar"
        VERBATIM)
    message(STATUS "MovieBackend: 'diagrams' target available (java -jar $ENV{PLANTUML_JAR})")
else()
    message(STATUS "MovieBackend: plantuml not found - 'diagrams' target unavailable "
                   "(set PLANTUML_JAR or install plantuml)")
endif()
