# tab5 port: libgooglepinyin Pinyin IME decoder as a reusable ESP-IDF component.
# Builder-only sources (dictbuilder/spellingtable/utf16reader) are kept in
# src/ for the host dictbuilder build (tools/ime_host) but excluded here;
# their code is additionally gated by IME_PINYIN_BUILD_MODEL (see dictdef.h).
idf_component_register(
    SRCS
        "src/dictlist.cpp"
        "src/dicttrie.cpp"
        "src/lpicache.cpp"
        "src/matrixsearch.cpp"
        "src/mystdlib.cpp"
        "src/ngram.cpp"
        "src/pinyinime.cpp"
        "src/searchutility.cpp"
        "src/spellingtrie.cpp"
        "src/splparser.cpp"
        "src/sync.cpp"
        "src/userdict.cpp"
        "src/utf16char.cpp"
    INCLUDE_DIRS "include"
    REQUIRES pthread
)

# Upstream code has benign sign-compare/unused warnings; don't let -Werror
# variants break the build. Format issues with size_t were fixed at source.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-sign-compare
)
