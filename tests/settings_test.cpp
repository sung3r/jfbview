#include "../src/settings.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <string>

#include "rapidjson/prettywriter.h"
#include "rapidjson/rapidjson.h"

class TempFile {
 public:
  const std::string FilePath;
  FILE* const FilePtr;

  static TempFile* Create() {
    char file_path_buffer[] = "/tmp/jfbview-test.XXXXXX";
    int fd = mkstemp(file_path_buffer);
    EXPECT_GT(fd, 1);
    return new TempFile(file_path_buffer, fdopen(fd, "r+"));
  }

  ~TempFile() {
    if (FilePtr != nullptr) {
      EXPECT_EQ(fclose(FilePtr), 0);
    }
    EXPECT_EQ(unlink(FilePath.c_str()), 0);
  }

 private:
  TempFile(const std::string& file_path, FILE* file_ptr)
      : FilePath(file_path), FilePtr(file_ptr) {}
  TempFile(const TempFile& other);
};

class SettingsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    _config_file.reset(TempFile::Create());
    _history_file.reset(TempFile::Create());
    ReloadSettings();
  }

  void TearDown() override {
    _settings.reset();
    _history_file.reset();
    _config_file.reset();
  }

  void ReloadSettings() {
    EXPECT_EQ(fflush(_config_file->FilePtr), 0);
    EXPECT_EQ(fflush(_history_file->FilePtr), 0);
    _settings.reset(
        Settings::Open(_config_file->FilePath, _history_file->FilePath));
  }

  std::unique_ptr<TempFile> _config_file;
  std::unique_ptr<TempFile> _history_file;
  std::unique_ptr<Settings> _settings;
};

TEST_F(SettingsTest, CanLoadDefaultSettings) {
  const rapidjson::Document& default_config = Settings::GetDefaultConfig();
  rapidjson::StringBuffer output_buffer;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(output_buffer);
  default_config.Accept(writer);
  const std::string output = output_buffer.GetString();
  EXPECT_GT(output.length(), 2);
  fprintf(stderr, "Loaded default settings:\n%s\n", output.c_str());
}

TEST_F(SettingsTest, GetValuesWithEmptyConfig) {
  const std::string fb = _settings->GetString("fb");
  EXPECT_GT(fb.length(), 0);
  EXPECT_EQ(fb, Settings::GetDefaultConfig()["fb"].GetString());

  int cache_size = _settings->GetInt("cacheSize");
  EXPECT_GT(cache_size, 0);
  EXPECT_EQ(cache_size, Settings::GetDefaultConfig()["cacheSize"].GetInt());
}

TEST_F(SettingsTest, GetValuesWithCustomConfig) {
  const char* custom_fb_value = "/dev/foobar";
  const int custom_cache_size = 42;
  fprintf(
      _config_file->FilePtr, "{\"fb\": \"%s\", \"cacheSize\": %d}",
      custom_fb_value, custom_cache_size);

  ReloadSettings();

  const std::string fb = _settings->GetString("fb");
  EXPECT_EQ(fb, custom_fb_value);

  const int cache_size = _settings->GetInt("cacheSize");
  EXPECT_EQ(cache_size, custom_cache_size);
}

