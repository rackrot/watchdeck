# Builds font_licenses.txt in the build tree: for each font in fonts/fonts.json, a header (name,
# copyright, license id) followed by the license text from fonts/<id>/. main/font_licenses.c
# embeds the file as `_binary_font_licenses_txt_start`. Nothing is copied into the source tree.
set(WD_FONTS_DIR ${CMAKE_CURRENT_LIST_DIR}/../../fonts)
set(WD_FONT_LICENSES_TXT ${CMAKE_CURRENT_BINARY_DIR}/font_licenses.txt)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    file(READ ${WD_FONTS_DIR}/fonts.json WD_FONTS_JSON)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${WD_FONTS_DIR}/fonts.json)

    set(_txt "Fonts bundled in this firmware. Each text below is the font's own license.\n")
    string(JSON _n LENGTH "${WD_FONTS_JSON}" fonts)
    math(EXPR _last "${_n} - 1")
    foreach(_i RANGE ${_last})
        string(JSON _id GET "${WD_FONTS_JSON}" fonts ${_i} id)
        string(JSON _name GET "${WD_FONTS_JSON}" fonts ${_i} name)
        string(JSON _copyright GET "${WD_FONTS_JSON}" fonts ${_i} copyright)
        string(JSON _spdx GET "${WD_FONTS_JSON}" fonts ${_i} license spdx)
        string(JSON _file GET "${WD_FONTS_JSON}" fonts ${_i} license file)
        set(_path ${WD_FONTS_DIR}/${_id}/${_file})
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_path})
        file(READ ${_path} _license)
        string(REPLACE "\t" "    " _license "${_license}") # Montserrat has no tab glyph

        string(APPEND _txt "\n================================================================\n"
                           "${_name}\n${_copyright}\nLicense: ${_spdx}\n")
        string(JSON _fallback ERROR_VARIABLE _none GET "${WD_FONTS_JSON}" fonts ${_i} fallback font)
        if(_fallback) # NOTFOUND (false) when the font has none
            set(_fallback_name ${_fallback})
            foreach(_j RANGE ${_last})
                string(JSON _jid GET "${WD_FONTS_JSON}" fonts ${_j} id)
                if(_jid STREQUAL _fallback)
                    string(JSON _fallback_name GET "${WD_FONTS_JSON}" fonts ${_j} name)
                endif()
            endforeach()
            string(APPEND _txt "Includes the \"^\" glyph from ${_fallback_name}; see its license below.\n")
        endif()
        string(APPEND _txt "================================================================\n\n${_license}")
    endforeach()

    # Rewrite only on a change, so a reconfigure doesn't relink the image.
    set(_old "")
    if(EXISTS ${WD_FONT_LICENSES_TXT})
        file(READ ${WD_FONT_LICENSES_TXT} _old)
    endif()
    if(NOT _old STREQUAL _txt)
        file(WRITE ${WD_FONT_LICENSES_TXT} "${_txt}")
    endif()
endif()
