// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/client/client.h"
#include "yb/client/client-internal.h"
#include "yb/client/meta_cache.h"
#include "yb/integration-tests/mini_cluster.h"
#include "yb/master/master.pb.h"
#include "yb/master/master.proxy.h"
#include "yb/master/mini_master.h"
#include "yb/rpc/messenger.h"
#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"
#include "yb/util/test_util.h"

namespace yb {
namespace client {

class PlacementInfoTest : public YBTest {
 public:
  PlacementInfoTest() {}

  ~PlacementInfoTest() {}

  const int kNumTservers = 3;

 protected:
  void SetUp() override {
    YBTest::SetUp();
    MiniClusterOptions opts;
    opts.num_masters = 1;
    opts.num_tablet_servers = kNumTservers;

    // Start tservers with different placement information.
    std::vector<tserver::TabletServerOptions> tserver_opts;
    for (int i = 0; i < kNumTservers; i++) {
      tserver::TabletServerOptions opts;
      opts.placement_cloud = "aws";
      opts.placement_region = PlacementRegion(i);
      opts.placement_zone = PlacementZone(i);
      tserver_opts.push_back(opts);
    }

    cluster_.reset(new MiniCluster(env_.get(), opts));
    ASSERT_OK(cluster_->Start(tserver_opts));
    for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
      std::string ts_uuid = cluster_->mini_tablet_server(i)->server()->fs_manager()->uuid();
      ts_uuid_to_index_.emplace(ts_uuid, i);
    }

    YBClientBuilder builder;
    ASSERT_OK(cluster_->CreateClient(&builder, &client_));
    rpc::MessengerBuilder bld("Client");
    ASSERT_OK(bld.Build().MoveTo(&client_messenger_));
    proxy_.reset(new master::MasterServiceProxy(client_messenger_,
                                                cluster_->leader_mini_master()->bound_rpc_addr()));

    // Create the table.
    YBSchema schema;
    YBSchemaBuilder b;
    b.AddColumn("key")->Type(INT32)->NotNull()->PrimaryKey();
    b.AddColumn("int_val")->Type(INT32)->NotNull();
    CHECK_OK(b.Build(&schema));
    gscoped_ptr<YBTableCreator> table_creator(client_->NewTableCreator());
    table_name_ = std::make_unique<YBTableName>("test_tablet_locations");
    table_name_->set_namespace_name(yb::master::kSystemNamespaceName);
    CHECK_OK(table_creator->table_name(*table_name_)
                 .schema(&schema)
                 .wait(true)
                 .num_tablets(1)
                 .num_replicas(kNumTservers)
                 .Create());

  }

  std::string PlacementRegion(int ts_index) {
    return strings::Substitute("region$0", ts_index);
  }

  std::string PlacementZone(int ts_index) {
    return strings::Substitute("zone$0", ts_index);
  }

  void TearDown() override {
    if (cluster_) {
      cluster_->Shutdown();
      cluster_.reset();
    }

    YBTest::TearDown();
  }

  void GetTabletLocations(master::TabletLocationsPB* tablet_locations) {
    // Retrieve tablets.
    rpc::RpcController controller;
    master::GetTableLocationsRequestPB req;
    master::GetTableLocationsResponsePB resp;
    table_name_->SetIntoTableIdentifierPB(req.mutable_table());
    ASSERT_OK(proxy_->GetTableLocations(req, &resp, &controller));

    // Verify tablet information.
    ASSERT_EQ(1, resp.tablet_locations_size());
    *tablet_locations = resp.tablet_locations(0);
    ASSERT_EQ(kNumTservers, tablet_locations->replicas_size());
  }

  void ValidateSelectTServer(const std::string& client_uuid, const std::string& placement_zone,
                             const std::string& placement_region,
                             int expected_ts_index,
                             internal::RemoteTablet* remote_tablet) {
    std::shared_ptr<client::YBClient> client;
    CloudInfoPB cloud_info;
    cloud_info.set_placement_zone(placement_zone);
    cloud_info.set_placement_region(placement_region);

    YBClientBuilder client_builder;
    client_builder.set_tserver_uuid(client_uuid);
    client_builder.set_cloud_info_pb(cloud_info);
    client_builder.add_master_server_addr(cluster_->leader_mini_master()->bound_rpc_addr_str());
    CHECK_OK(client_builder.Build(&client));

    // Select tserver.
    vector<internal::RemoteTabletServer *> candidates;
    internal::RemoteTabletServer *tserver = client->data_->SelectTServer(
        remote_tablet, YBClient::ReplicaSelection::CLOSEST_REPLICA, std::set<string>(),
        &candidates);
    ASSERT_EQ(expected_ts_index, ts_uuid_to_index_[tserver->permanent_uuid()]);
  }

  std::unique_ptr<MiniCluster> cluster_;
  std::shared_ptr<YBClient> client_;
  std::unique_ptr<master::MasterServiceProxy> proxy_;
  std::shared_ptr<rpc::Messenger> client_messenger_;
  std::map<std::string, int> ts_uuid_to_index_;
  std::unique_ptr<YBTableName> table_name_;
};

TEST_F(PlacementInfoTest, TestTabletLocations) {
  master::TabletLocationsPB tablet_locations;
  GetTabletLocations(&tablet_locations);
  for (int i = 0; i < tablet_locations.replicas_size(); i++) {
    // Find the ts by uuid.
    auto cloud_info = tablet_locations.replicas(i).ts_info().cloud_info();
    auto ts_uuid = tablet_locations.replicas(i).ts_info().permanent_uuid();
    auto it = ts_uuid_to_index_.find(ts_uuid);
    int ts_index = (*it).second;
    ASSERT_TRUE(it != ts_uuid_to_index_.end());

    // Remove it from the map to ensure we look through all ts uuids.
    ts_uuid_to_index_.erase(it);

    ASSERT_EQ("aws", cloud_info.placement_cloud());
    ASSERT_EQ(PlacementRegion(ts_index), cloud_info.placement_region());
    ASSERT_EQ(PlacementZone(ts_index), cloud_info.placement_zone());
  }
  ASSERT_EQ(0, ts_uuid_to_index_.size());
}

TEST_F(PlacementInfoTest, TestSelectTServer) {
  master::TabletLocationsPB tablet_locations;
  GetTabletLocations(&tablet_locations);

  Partition partition;
  Partition::FromPB(tablet_locations.partition(), &partition);
  internal::RemoteTabletPtr remote_tablet = new internal::RemoteTablet(
      tablet_locations.tablet_id(), partition);

  // Build remote tserver map.
  internal::TabletServerMap tserver_map;
  for (const master::TabletLocationsPB::ReplicaPB& replica : tablet_locations.replicas()) {
    tserver_map.emplace(replica.ts_info().permanent_uuid(),
                        std::make_unique<internal::RemoteTabletServer>(replica.ts_info()));
  }

  // Refresh replicas for RemoteTablet.
  remote_tablet->Refresh(tserver_map, tablet_locations.replicas());

  for (int ts_index = 0; ts_index < kNumTservers; ts_index++) {
    ValidateSelectTServer(
        cluster_->mini_tablet_server(ts_index)->server()->permanent_uuid(), "", "", ts_index,
        remote_tablet.get());
    ValidateSelectTServer("", PlacementZone(ts_index), "", ts_index, remote_tablet.get());
    ValidateSelectTServer("", "", PlacementRegion(ts_index), ts_index, remote_tablet.get());
    ValidateSelectTServer("", PlacementZone(ts_index),
                          PlacementRegion((ts_index + 1) % kNumTservers), ts_index,
                          remote_tablet.get());
    ValidateSelectTServer(
        cluster_->mini_tablet_server(ts_index)->server()->permanent_uuid(),
        PlacementZone((ts_index + 1) % kNumTservers),
        PlacementRegion((ts_index + 2) % kNumTservers), ts_index, remote_tablet.get());
  }
}

} // namespace client
} // namespace yb
