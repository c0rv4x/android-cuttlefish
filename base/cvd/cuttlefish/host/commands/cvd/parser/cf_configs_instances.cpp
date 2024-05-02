/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "host/commands/cvd/parser/cf_configs_instances.h"

#include <string>
#include <vector>

#include <android-base/logging.h>
#include "json/json.h"

#include "common/libs/utils/result.h"
#include "cuttlefish/host/commands/cvd/parser/load_config.pb.h"
#include "host/commands/cvd/parser/cf_configs_common.h"
#include "host/commands/cvd/parser/instance/cf_boot_configs.h"
#include "host/commands/cvd/parser/instance/cf_connectivity_configs.h"
#include "host/commands/cvd/parser/instance/cf_disk_configs.h"
#include "host/commands/cvd/parser/instance/cf_graphics_configs.h"
#include "host/commands/cvd/parser/instance/cf_security_configs.h"
#include "host/commands/cvd/parser/instance/cf_streaming_configs.h"
#include "host/commands/cvd/parser/instance/cf_vm_configs.h"

namespace cuttlefish {

Result<void> InitInstancesConfigs(Json::Value& instances) {
  for (auto& instance : instances) {
    CF_EXPECT(InitConfig(instance, "", {"name"}));
  }
  CF_EXPECT(InitBootConfigs(instances));
  CF_EXPECT(InitDiskConfigs(instances));
  CF_EXPECT(InitGraphicsConfigs(instances));
  CF_EXPECT(InitSecurityConfigs(instances));
  CF_EXPECT(InitStreamingConfigs(instances));
  CF_EXPECT(InitVmConfigs(instances));
  return {};
}

Result<std::vector<std::string>> GenerateInstancesFlags(
    const Json::Value& instances, const cvd::config::Launch& config) {
  std::vector<std::string> result = CF_EXPECT(GenerateBootFlags(config));
  result = MergeResults(std::move(result), GenerateDiskFlags(config));
  result = MergeResults(result, CF_EXPECT(GenerateGraphicsFlags(config)));
  result = MergeResults(std::move(result), GenerateSecurityFlags(config));
  result = MergeResults(std::move(result), GenerateStreamingFlags(config));
  result = MergeResults(result, CF_EXPECT(GenerateVmFlags(config)));
  result = MergeResults(std::move(result), GenerateConnectivityFlags(config));

  return result;
}

}  // namespace cuttlefish
