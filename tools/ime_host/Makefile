# Host build of the ported ime_pinyin component (no cmake required).
# tab5 port: sources now come from components/ime_pinyin (single source of
# truth), NOT the pristine libgooglepinyin/ clone kept here for reference.
#
#   make           # dictbuilder + v2-uint32 dict + test_cli (component build)
#   make test      # regression: file-open AND im_open_decoder_mem paths
CXX        := clang++
ENGINE     := ../../components/ime_pinyin
INC        := -I$(ENGINE)/include
# Builder build: engine + builder code (IME_PINYIN_BUILD_MODEL re-enables
# ___BUILD_MODEL___, see dictdef.h)
CXXFLAGS_B := -std=c++14 -O2 -w $(INC) -DIME_PINYIN_BUILD_MODEL
# Component build: exactly what the ESP-IDF component compiles (decoder only),
# with the format warnings IDF would enforce.
CXXFLAGS_C := -std=c++14 -O2 -Wall -Wformat -Wno-unused-variable \
              -Wno-unused-but-set-variable -Wno-sign-compare $(INC)

ALL_SRC    := $(wildcard $(ENGINE)/src/*.cpp)
# Same list as components/ime_pinyin/CMakeLists.txt SRCS:
COMP_SRC   := $(addprefix $(ENGINE)/src/, dictlist.cpp dicttrie.cpp \
              lpicache.cpp matrixsearch.cpp mystdlib.cpp ngram.cpp \
              pinyinime.cpp searchutility.cpp spellingtrie.cpp splparser.cpp \
              sync.cpp userdict.cpp utf16char.cpp)

OBJ_B := $(patsubst $(ENGINE)/src/%.cpp,build/builder/%.o,$(ALL_SRC))
OBJ_C := $(patsubst $(ENGINE)/src/%.cpp,build/comp/%.o,$(COMP_SRC))

all: build/pinyinime_dictbuilder build/dict_pinyin.dat build/test_cli

build/builder/%.o: $(ENGINE)/src/%.cpp | dirs
	$(CXX) $(CXXFLAGS_B) -c $< -o $@

build/comp/%.o: $(ENGINE)/src/%.cpp | dirs
	$(CXX) $(CXXFLAGS_C) -c $< -o $@

dirs:
	mkdir -p build/builder build/comp

build/pinyinime_dictbuilder: libgooglepinyin/tools/pinyinime_dictbuilder.cpp $(OBJ_B)
	$(CXX) $(CXXFLAGS_B) $^ -o $@

build/dict_pinyin.dat: build/pinyinime_dictbuilder
	cd build && ./pinyinime_dictbuilder ../libgooglepinyin/data/rawdict_utf16_65105_freq.txt ../libgooglepinyin/data/valid_utf16.txt dict_pinyin.dat

# test_cli links against the COMPONENT objects (proves the ported component)
build/test_cli: test_cli.cpp $(OBJ_C)
	$(CXX) $(CXXFLAGS_C) $^ -o $@

test: all
	cd build && ./test_cli dict_pinyin.dat nihao \
	  && ./test_cli dict_pinyin.dat zhongguo \
	  && ./test_cli dict_pinyin.dat zhg \
	  && ./test_cli dict_pinyin.dat woshizhongguoren --choose \
	  && ./test_cli dict_pinyin.dat nihao --mem \
	  && ./test_cli dict_pinyin.dat zhongguo --mem \
	  && ./test_cli dict_pinyin.dat woshizhongguoren --choose --mem \
	  && ./test_cli dict_pinyin.dat nihao --inc --mem

clean:
	rm -rf build

.PHONY: all test clean dirs
