// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 * Copyright (C) 2013,2014 Cloudwatt <libre.licensing@cloudwatt.com>
 *
 * Author: Loic Dachary <loic@dachary.org>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */


#ifndef CEPH_OSDMAP_H
#define CEPH_OSDMAP_H

/*
 * describe properties of the OSD cluster.
 *   disks, disk groups, total # osds,
 *
 */
#include "include/types.h"
#include "osd_types.h"

//#include "include/ceph_features.h"
#include "crush/CrushWrapper.h"
#include <vector>
#include <list>
#include <set>
#include <map>
#include "include/memory.h"
using namespace std;

// forward declaration
class CephContext;
class CrushWrapper;

// FIXME C++11 does not have std::equal for two differently-typed containers.
// use this until we move to c++14
template<typename A, typename B>
bool vectors_equal(A a, B b)
{
  return
    a.size() == b.size() &&
    (a.empty() ||
     memcmp((char*)&a[0], (char*)&b[0], sizeof(a[0]) * a.size()) == 0);
}


/*
 * we track up to two intervals during which the osd was alive and
 * healthy.  the most recent is [up_from,up_thru), where up_thru is
 * the last epoch the osd is known to have _started_.  i.e., a lower
 * bound on the actual osd death.  down_at (if it is > up_from) is an
 * upper bound on the actual osd death.
 *
 * the second is the last_clean interval [first,last].  in that case,
 * the last interval is the last epoch known to have been either
 * _finished_, or during which the osd cleanly shut down.  when
 * possible, we push this forward to the epoch the osd was eventually
 * marked down.
 *
 * the lost_at is used to allow build_prior to proceed without waiting
 * for an osd to recover.  In certain cases, progress may be blocked 
 * because an osd is down that may contain updates (i.e., a pg may have
 * gone rw during an interval).  If the osd can't be brought online, we
 * can force things to proceed knowing that we _might_ be losing some
 * acked writes.  If the osd comes back to life later, that's fine to,
 * but those writes will still be lost (the divergent objects will be
 * thrown out).
 */
struct osd_info_t {
  epoch_t last_clean_begin;  // last interval that ended with a clean osd shutdown
  epoch_t last_clean_end;
  epoch_t up_from;   // epoch osd marked up
  epoch_t up_thru;   // lower bound on actual osd death (if > up_from)
  epoch_t down_at;   // upper bound on actual osd death (if > up_from)
  epoch_t lost_at;   // last epoch we decided data was "lost"
  
  osd_info_t() : last_clean_begin(0), last_clean_end(0),
		 up_from(0), up_thru(0), down_at(0), lost_at(0) {}

  void dump(Formatter *f) const;
  void encode(bufferlist& bl) const;
  void decode(bufferlist::iterator& bl);
  static void generate_test_instances(list<osd_info_t*>& o);
};
WRITE_CLASS_ENCODER(osd_info_t)

ostream& operator<<(ostream& out, const osd_info_t& info);

struct osd_xinfo_t {
  utime_t down_stamp;      ///< timestamp when we were last marked down
  float laggy_probability; ///< encoded as __u32: 0 = definitely not laggy, 0xffffffff definitely laggy
  __u32 laggy_interval;    ///< average interval between being marked laggy and recovering
  uint64_t features;       ///< features supported by this osd we should know about
  __u32 old_weight;        ///< weight prior to being auto marked out

  osd_xinfo_t() : laggy_probability(0), laggy_interval(0),
                  features(0), old_weight(0) {}

  void dump(Formatter *f) const;
  void encode(bufferlist& bl) const;
  void decode(bufferlist::iterator& bl);
  static void generate_test_instances(list<osd_xinfo_t*>& o);
};
WRITE_CLASS_ENCODER(osd_xinfo_t)

ostream& operator<<(ostream& out, const osd_xinfo_t& xi);


/** OSDMap
 */
class OSDMap {
public:
  MEMPOOL_CLASS_HELPERS();

  class Incremental {
  public:
    MEMPOOL_CLASS_HELPERS();

    /// feature bits we were encoded with.  the subsequent OSDMap
    /// encoding should match.
    uint64_t encode_features;
    uuid_d fsid;
    epoch_t epoch;   // new epoch; we are a diff from epoch-1 to epoch
    utime_t modified;
    int64_t new_pool_max; //incremented by the OSDMonitor on each pool create
    int32_t new_flags;

    // full (rare)
    bufferlist fullmap;  // in lieu of below.
    bufferlist crush;

    // incremental
    int32_t new_max_osd;
    mempool::osdmap::map<int64_t,pg_pool_t> new_pools;
    mempool::osdmap::map<int64_t,string> new_pool_names;
    mempool::osdmap::set<int64_t> old_pools;
    mempool::osdmap::map<string,map<string,string> > new_erasure_code_profiles;
    mempool::osdmap::vector<string> old_erasure_code_profiles;
    mempool::osdmap::map<int32_t,entity_addr_t> new_up_client;
    mempool::osdmap::map<int32_t,entity_addr_t> new_up_cluster;
    mempool::osdmap::map<int32_t,uint8_t> new_state;             // XORed onto previous state.
    mempool::osdmap::map<int32_t,uint32_t> new_weight;
    mempool::osdmap::map<pg_t,mempool::osdmap::vector<int32_t> > new_pg_temp;     // [] to remove
    mempool::osdmap::map<pg_t, int32_t> new_primary_temp;            // [-1] to remove
    mempool::osdmap::map<int32_t,uint32_t> new_primary_affinity;
    mempool::osdmap::map<int32_t,epoch_t> new_up_thru;
    mempool::osdmap::map<int32_t,pair<epoch_t,epoch_t> > new_last_clean_interval;
    mempool::osdmap::map<int32_t,epoch_t> new_lost;
    mempool::osdmap::map<int32_t,uuid_d> new_uuid;
    mempool::osdmap::map<int32_t,osd_xinfo_t> new_xinfo;

    mempool::osdmap::map<entity_addr_t,utime_t> new_blacklist;
    mempool::osdmap::vector<entity_addr_t> old_blacklist;
    mempool::osdmap::map<int32_t, entity_addr_t> new_hb_back_up;
    mempool::osdmap::map<int32_t, entity_addr_t> new_hb_front_up;

    mempool::osdmap::map<pg_t,mempool::osdmap::vector<int32_t>> new_pg_upmap;
    mempool::osdmap::map<pg_t,mempool::osdmap::vector<pair<int32_t,int32_t>>> new_pg_upmap_items;
    mempool::osdmap::set<pg_t> old_pg_upmap, old_pg_upmap_items;

    string cluster_snapshot;

    float new_nearfull_ratio = -1;
    float new_backfillfull_ratio = -1;
    float new_full_ratio = -1;

    string new_require_min_compat_client;

    mutable bool have_crc;      ///< crc values are defined
    uint32_t full_crc;  ///< crc of the resulting OSDMap
    mutable uint32_t inc_crc;   ///< crc of this incremental

    int get_net_marked_out(const OSDMap *previous) const;
    int get_net_marked_down(const OSDMap *previous) const;
    int identify_osd(uuid_d u) const;

    void encode_client_old(bufferlist& bl) const;
    void encode_classic(bufferlist& bl, uint64_t features) const;
    void encode(bufferlist& bl, uint64_t features=CEPH_FEATURES_ALL) const;
    void decode_classic(bufferlist::iterator &p);
    void decode(bufferlist::iterator &bl);
    void dump(Formatter *f) const;
    static void generate_test_instances(list<Incremental*>& o);

    explicit Incremental(epoch_t e=0) :
      encode_features(0),
      epoch(e), new_pool_max(-1), new_flags(-1), new_max_osd(-1),
      have_crc(false), full_crc(0), inc_crc(0) {
      memset(&fsid, 0, sizeof(fsid));
    }
    explicit Incremental(bufferlist &bl) {
      bufferlist::iterator p = bl.begin();
      decode(p);
    }
    explicit Incremental(bufferlist::iterator &p) {
      decode(p);
    }

    pg_pool_t *get_new_pool(int64_t pool, const pg_pool_t *orig) {
      if (new_pools.count(pool) == 0)
	new_pools[pool] = *orig;
      return &new_pools[pool];
    }
    bool has_erasure_code_profile(const string &name) const {
      auto i = new_erasure_code_profiles.find(name);
      return i != new_erasure_code_profiles.end();
    }
    void set_erasure_code_profile(const string &name,
				  const map<string,string>& profile) {
      new_erasure_code_profiles[name] = profile;
    }

    /// propage update pools' snap metadata to any of their tiers
    int propagate_snaps_to_tiers(CephContext *cct, const OSDMap &base);
  };
  
private:
  uuid_d fsid;
  epoch_t epoch;        // what epoch of the osd cluster descriptor is this
  utime_t created, modified; // epoch start time
  int32_t pool_max;     // the largest pool num, ever

  uint32_t flags;

  int num_osd;         // not saved; see calc_num_osds
  int num_up_osd;      // not saved; see calc_num_osds
  int num_in_osd;      // not saved; see calc_num_osds

  int32_t max_osd;
  vector<uint8_t> osd_state;

  struct addrs_s {
    mempool::osdmap::vector<ceph::shared_ptr<entity_addr_t> > client_addr;
    mempool::osdmap::vector<ceph::shared_ptr<entity_addr_t> > cluster_addr;
    mempool::osdmap::vector<ceph::shared_ptr<entity_addr_t> > hb_back_addr;
    mempool::osdmap::vector<ceph::shared_ptr<entity_addr_t> > hb_front_addr;
    entity_addr_t blank;
  };
  ceph::shared_ptr<addrs_s> osd_addrs;

  mempool::osdmap::vector<__u32>   osd_weight;   // 16.16 fixed point, 0x10000 = "in", 0 = "out"
  mempool::osdmap::vector<osd_info_t> osd_info;
  ceph::shared_ptr< mempool::osdmap::map<pg_t,mempool::osdmap::vector<int32_t> > > pg_temp;  // temp pg mapping (e.g. while we rebuild)
  ceph::shared_ptr< mempool::osdmap::map<pg_t,int32_t > > primary_temp;  // temp primary mapping (e.g. while we rebuild)
  ceph::shared_ptr< mempool::osdmap::vector<__u32> > osd_primary_affinity; ///< 16.16 fixed point, 0x10000 = baseline

  // remap (post-CRUSH, pre-up)
  mempool::osdmap::map<pg_t,mempool::osdmap::vector<int32_t>> pg_upmap; ///< remap pg
  mempool::osdmap::map<pg_t,mempool::osdmap::vector<pair<int32_t,int32_t>>> pg_upmap_items; ///< remap osds in up set

  mempool::osdmap::map<int64_t,pg_pool_t> pools;
  mempool::osdmap::map<int64_t,string> pool_name;
  mempool::osdmap::map<string,map<string,string> > erasure_code_profiles;
  mempool::osdmap::map<string,int64_t> name_pool;

  ceph::shared_ptr< mempool::osdmap::vector<uuid_d> > osd_uuid;
  mempool::osdmap::vector<osd_xinfo_t> osd_xinfo;

  mempool::osdmap::unordered_map<entity_addr_t,utime_t> blacklist;

  epoch_t cluster_snapshot_epoch;
  string cluster_snapshot;
  bool new_blacklist_entries;

  float full_ratio = 0, backfillfull_ratio = 0, nearfull_ratio = 0;

  /// min compat client we want to support
  string require_min_compat_client;

  mutable uint64_t cached_up_osd_features;

  mutable bool crc_defined;
  mutable uint32_t crc;

  void _calc_up_osd_features();

 public:
  bool have_crc() const { return crc_defined; }
  uint32_t get_crc() const { return crc; }

  ceph::shared_ptr<CrushWrapper> crush;       // hierarchical map

  friend class OSDMonitor;

 public:
  OSDMap() : epoch(0), 
	     pool_max(-1),
	     flags(0),
	     num_osd(0), num_up_osd(0), num_in_osd(0),
	     max_osd(0),
	     osd_addrs(std::make_shared<addrs_s>()),
	     pg_temp(std::make_shared<mempool::osdmap::map<pg_t,mempool::osdmap::vector<int32_t>>>()),
	     primary_temp(std::make_shared<mempool::osdmap::map<pg_t,int32_t>>()),
	     osd_uuid(std::make_shared<mempool::osdmap::vector<uuid_d>>()),
	     cluster_snapshot_epoch(0),
	     new_blacklist_entries(false),
	     cached_up_osd_features(0),
	     crc_defined(false), crc(0),
	     crush(std::make_shared<CrushWrapper>()) {
    memset(&fsid, 0, sizeof(fsid));
  }

  // no copying
private:
  OSDMap(const OSDMap& other) = default;
  OSDMap& operator=(const OSDMap& other) = default;
public:

  void deepish_copy_from(const OSDMap& o) {
    *this = o;
    primary_temp.reset(new mempool::osdmap::map<pg_t,int32_t>(*o.primary_temp));
    pg_temp.reset(new mempool::osdmap::map<pg_t,mempool::osdmap::vector<int32_t> >(*o.pg_temp));
    osd_uuid.reset(new mempool::osdmap::vector<uuid_d>(*o.osd_uuid));

    if (o.osd_primary_affinity)
      osd_primary_affinity.reset(new mempool::osdmap::vector<__u32>(*o.osd_primary_affinity));

    // NOTE: this still references shared entity_addr_t's.
    osd_addrs.reset(new addrs_s(*o.osd_addrs));

    // NOTE: we do not copy crush.  note that apply_incremental will
    // allocate a new CrushWrapper, though.
  }

  // map info
  const uuid_d& get_fsid() const { return fsid; }
  void set_fsid(uuid_d& f) { fsid = f; }

  epoch_t get_epoch() const { return epoch; }
  void inc_epoch() { epoch++; }

  void set_epoch(epoch_t e);

  /* stamps etc */
  const utime_t& get_created() const { return created; }
  const utime_t& get_modified() const { return modified; }

  bool is_blacklisted(const entity_addr_t& a) const;
  void get_blacklist(list<pair<entity_addr_t,utime_t > > *bl) const;

  string get_cluster_snapshot() const {
    if (cluster_snapshot_epoch == epoch)
      return cluster_snapshot;
    return string();
  }

  float get_full_ratio() const {
    return full_ratio;
  }
  float get_backfillfull_ratio() const {
    return backfillfull_ratio;
  }
  float get_nearfull_ratio() const {
    return nearfull_ratio;
  }
  void count_full_nearfull_osds(int *full, int *backfill, int *nearfull) const;
  void get_full_osd_util(
    const ceph::unordered_map<int32_t,osd_stat_t> &osd_stat,
    map<int, float> *full,
    map<int, float> *backfill,
    map<int, float> *nearfull) const;

  /***** cluster state *****/
  /* osds */
  int get_max_osd() const { return max_osd; }
  void set_max_osd(int m);

  unsigned get_num_osds() const {
    return num_osd;
  }
  unsigned get_num_up_osds() const {
    return num_up_osd;
  }
  unsigned get_num_in_osds() const {
    return num_in_osd;
  }
  /// recalculate cached values for get_num{,_up,_in}_osds
  int calc_num_osds();

  void get_all_osds(set<int32_t>& ls) const;
  void get_up_osds(set<int32_t>& ls) const;
  unsigned get_num_pg_temp() const {
    return pg_temp->size();
  }

  int get_flags() const { return flags; }
  bool test_flag(int f) const { return flags & f; }
  void set_flag(int f) { flags |= f; }
  void clear_flag(int f) { flags &= ~f; }

  static void calc_state_set(int state, set<string>& st);

  int get_state(int o) const {
    assert(o < max_osd);
    return osd_state[o];
  }
  int get_state(int o, set<string>& st) const {
    assert(o < max_osd);
    unsigned t = osd_state[o];
    calc_state_set(t, st);
    return osd_state[o];
  }
  void set_state(int o, unsigned s) {
    assert(o < max_osd);
    osd_state[o] = s;
  }
  void set_weight(int o, unsigned w) {
    assert(o < max_osd);
    osd_weight[o] = w;
    if (w)
      osd_state[o] |= CEPH_OSD_EXISTS;
  }
  unsigned get_weight(int o) const {
    assert(o < max_osd);
    return osd_weight[o];
  }
  float get_weightf(int o) const {
    return (float)get_weight(o) / (float)CEPH_OSD_IN;
  }
  void adjust_osd_weights(const map<int,double>& weights, Incremental& inc) const;

  void set_primary_affinity(int o, int w) {
    assert(o < max_osd);
    if (!osd_primary_affinity)
      osd_primary_affinity.reset(
	new mempool::osdmap::vector<__u32>(
	  max_osd, CEPH_OSD_DEFAULT_PRIMARY_AFFINITY));
    (*osd_primary_affinity)[o] = w;
  }
  unsigned get_primary_affinity(int o) const {
    assert(o < max_osd);
    if (!osd_primary_affinity)
      return CEPH_OSD_DEFAULT_PRIMARY_AFFINITY;
    return (*osd_primary_affinity)[o];
  }
  float get_primary_affinityf(int o) const {
    return (float)get_primary_affinity(o) / (float)CEPH_OSD_MAX_PRIMARY_AFFINITY;
  }

  bool has_erasure_code_profile(const string &name) const {
    auto i = erasure_code_profiles.find(name);
    return i != erasure_code_profiles.end();
  }
  int get_erasure_code_profile_default(CephContext *cct,
				       map<string,string> &profile_map,
				       ostream *ss);
  void set_erasure_code_profile(const string &name,
				const map<string,string>& profile) {
    erasure_code_profiles[name] = profile;
  }
  const map<string,string> &get_erasure_code_profile(
    const string &name) const {
    static map<string,string> empty;
    auto i = erasure_code_profiles.find(name);
    if (i == erasure_code_profiles.end())
      return empty;
    else
      return i->second;
  }
  const mempool::osdmap::map<string,map<string,string> > &get_erasure_code_profiles() const {
    return erasure_code_profiles;
  }

  bool exists(int osd) const {
    //assert(osd >= 0);
    return osd >= 0 && osd < max_osd && (osd_state[osd] & CEPH_OSD_EXISTS);
  }

  bool is_up(int osd) const {
    return exists(osd) && (osd_state[osd] & CEPH_OSD_UP);
  }

  bool has_been_up_since(int osd, epoch_t epoch) const {
    return is_up(osd) && get_up_from(osd) <= epoch;
  }

  bool is_down(int osd) const {
    return !is_up(osd);
  }

  bool is_out(int osd) const {
    return !exists(osd) || get_weight(osd) == CEPH_OSD_OUT;
  }

  bool is_in(int osd) const {
    return !is_out(osd);
  }

  /**
   * check if an entire crush subtree is down
   */
  bool subtree_is_down(int id, set<int> *down_cache) const;
  bool containing_subtree_is_down(CephContext *cct, int osd, int subtree_type, set<int> *down_cache) const;
  
  int identify_osd(const entity_addr_t& addr) const;
  int identify_osd(const uuid_d& u) const;
  int identify_osd_on_all_channels(const entity_addr_t& addr) const;

  bool have_addr(const entity_addr_t& addr) const {
    return identify_osd(addr) >= 0;
  }
  int find_osd_on_ip(const entity_addr_t& ip) const;
  const entity_addr_t &get_addr(int osd) const {
    assert(exists(osd));
    return osd_addrs->client_addr[osd] ? *osd_addrs->client_addr[osd] : osd_addrs->blank;
  }
  const entity_addr_t &get_cluster_addr(int osd) const {
    assert(exists(osd));
    if (!osd_addrs->cluster_addr[osd] || *osd_addrs->cluster_addr[osd] == entity_addr_t())
      return get_addr(osd);
    return *osd_addrs->cluster_addr[osd];
  }
  const entity_addr_t &get_hb_back_addr(int osd) const {
    assert(exists(osd));
    return osd_addrs->hb_back_addr[osd] ? *osd_addrs->hb_back_addr[osd] : osd_addrs->blank;
  }
  const entity_addr_t &get_hb_front_addr(int osd) const {
    assert(exists(osd));
    return osd_addrs->hb_front_addr[osd] ? *osd_addrs->hb_front_addr[osd] : osd_addrs->blank;
  }
  entity_inst_t get_most_recent_inst(int osd) const {
    assert(exists(osd));
    return entity_inst_t(entity_name_t::OSD(osd), get_addr(osd));
  }
  entity_inst_t get_inst(int osd) const {
    assert(is_up(osd));
    return get_most_recent_inst(osd);
  }
  entity_inst_t get_cluster_inst(int osd) const {
    assert(is_up(osd));
    return entity_inst_t(entity_name_t::OSD(osd), get_cluster_addr(osd));
  }
  entity_inst_t get_hb_back_inst(int osd) const {
    assert(is_up(osd));
    return entity_inst_t(entity_name_t::OSD(osd), get_hb_back_addr(osd));
  }
  entity_inst_t get_hb_front_inst(int osd) const {
    assert(is_up(osd));
    return entity_inst_t(entity_name_t::OSD(osd), get_hb_front_addr(osd));
  }

  const uuid_d& get_uuid(int osd) const {
    assert(exists(osd));
    return (*osd_uuid)[osd];
  }

  const epoch_t& get_up_from(int osd) const {
    assert(exists(osd));
    return osd_info[osd].up_from;
  }
  const epoch_t& get_up_thru(int osd) const {
    assert(exists(osd));
    return osd_info[osd].up_thru;
  }
  const epoch_t& get_down_at(int osd) const {
    assert(exists(osd));
    return osd_info[osd].down_at;
  }
  const osd_info_t& get_info(int osd) const {
    assert(osd < max_osd);
    return osd_info[osd];
  }

  const osd_xinfo_t& get_xinfo(int osd) const {
    assert(osd < max_osd);
    return osd_xinfo[osd];
  }
  
  int get_next_up_osd_after(int n) const {
    if (get_max_osd() == 0)
      return -1;
    for (int i = n + 1; i != n; ++i) {
      if (i >= get_max_osd())
	i = 0;
      if (i == n)
	break;
      if (is_up(i))
	return i;
    }
    return -1;
  }

  int get_previous_up_osd_before(int n) const {
    if (get_max_osd() == 0)
      return -1;
    for (int i = n - 1; i != n; --i) {
      if (i < 0)
	i = get_max_osd() - 1;
      if (i == n)
	break;
      if (is_up(i))
	return i;
    }
    return -1;
  }

  /**
   * get feature bits required by the current structure
   *
   * @param entity_type [in] what entity type we are asking about
   * @param mask [out] set of all possible map-related features we could set
   * @return feature bits used by this map
   */
  uint64_t get_features(int entity_type, uint64_t *mask) const;

  /**
   * get oldest *client* version (firefly, hammer, etc.) that can connect given
   * the feature bits required (according to get_features()).
   */
  pair<string,string> get_min_compat_client() const;

  /**
   * get intersection of features supported by up osds
   */
  uint64_t get_up_osd_features() const;

  int apply_incremental(const Incremental &inc);

  /// try to re-use/reference addrs in oldmap from newmap
  static void dedup(const OSDMap *oldmap, OSDMap *newmap);

  static void clean_temps(CephContext *cct, const OSDMap& osdmap,
			  Incremental *pending_inc);

  // serialize, unserialize
private:
  void encode_client_old(bufferlist& bl) const;
  void encode_classic(bufferlist& bl, uint64_t features) const;
  void decode_classic(bufferlist::iterator& p);
  void post_decode();
public:
  void encode(bufferlist& bl, uint64_t features=CEPH_FEATURES_ALL) const;
  void decode(bufferlist& bl);
  void decode(bufferlist::iterator& bl);


  /****   mapping facilities   ****/
  int map_to_pg(
    int64_t pool,
    const string& name,
    const string& key,
    const string& nspace,
    pg_t *pg) const;
  int object_locator_to_pg(const object_t& oid, const object_locator_t& loc,
			   pg_t &pg) const;
  pg_t object_locator_to_pg(const object_t& oid,
			    const object_locator_t& loc) const {
    pg_t pg;
    int ret = object_locator_to_pg(oid, loc, pg);
    assert(ret == 0);
    return pg;
  }


  static object_locator_t file_to_object_locator(const file_layout_t& layout) {
    return object_locator_t(layout.pool_id, layout.pool_ns);
  }

  ceph_object_layout file_to_object_layout(object_t oid,
					   file_layout_t& layout) const {
    return make_object_layout(oid, layout.pool_id, layout.pool_ns);
  }

  ceph_object_layout make_object_layout(object_t oid, int pg_pool,
					string nspace) const;

  int get_pg_num(int pg_pool) const
  {
    const pg_pool_t *pool = get_pg_pool(pg_pool);
    assert(NULL != pool);
    return pool->get_pg_num();
  }

  bool pg_exists(pg_t pgid) const {
    const pg_pool_t *p = get_pg_pool(pgid.pool());
    return p && pgid.ps() < p->get_pg_num();
  }

private:
  /// pg -> (raw osd list)
  int _pg_to_raw_osds(
    const pg_pool_t& pool, pg_t pg,
    vector<int> *osds,
    ps_t *ppps) const;
  int _pick_primary(const vector<int>& osds) const;
  void _remove_nonexistent_osds(const pg_pool_t& pool, vector<int>& osds) const;

  void _apply_primary_affinity(ps_t seed, const pg_pool_t& pool,
			       vector<int> *osds, int *primary) const;

  /// apply pg_upmap[_items] mappings
  void _apply_remap(const pg_pool_t& pi, pg_t pg, vector<int> *raw) const;

  /// pg -> (up osd list)
  void _raw_to_up_osds(const pg_pool_t& pool, const vector<int>& raw,
                       vector<int> *up) const;


  /**
   * Get the pg and primary temp, if they are specified.
   * @param temp_pg [out] Will be empty or contain the temp PG mapping on return
   * @param temp_primary [out] Will be the value in primary_temp, or a value derived
   * from the pg_temp (if specified), or -1 if you should use the calculated (up_)primary.
   */
  void _get_temp_osds(const pg_pool_t& pool, pg_t pg,
                      vector<int> *temp_pg, int *temp_primary) const;

  /**
   *  map to up and acting. Fills in whatever fields are non-NULL.
   */
  void _pg_to_up_acting_osds(const pg_t& pg, vector<int> *up, int *up_primary,
                             vector<int> *acting, int *acting_primary,
			     bool raw_pg_to_pg = true) const;

public:
  /***
   * This is suitable only for looking at raw CRUSH outputs. It skips
   * applying the temp and up checks and should not be used
   * by anybody for data mapping purposes.
   * raw and primary must be non-NULL
   */
  int pg_to_raw_osds(pg_t pg, vector<int> *raw, int *primary) const;
  /// map a pg to its acting set. @return acting set size
  int pg_to_acting_osds(const pg_t& pg, vector<int> *acting,
                        int *acting_primary) const {
    _pg_to_up_acting_osds(pg, NULL, NULL, acting, acting_primary);
    return acting->size();
  }
  int pg_to_acting_osds(pg_t pg, vector<int>& acting) const {
    return pg_to_acting_osds(pg, &acting, NULL);
  }
  /**
   * This does not apply temp overrides and should not be used
   * by anybody for data mapping purposes. Specify both pointers.
   */
  void pg_to_raw_up(pg_t pg, vector<int> *up, int *primary) const;
  /**
   * map a pg to its acting set as well as its up set. You must use
   * the acting set for data mapping purposes, but some users will
   * also find the up set useful for things like deciding what to
   * set as pg_temp.
   * Each of these pointers must be non-NULL.
   */
  void pg_to_up_acting_osds(pg_t pg, vector<int> *up, int *up_primary,
                            vector<int> *acting, int *acting_primary) const {
    _pg_to_up_acting_osds(pg, up, up_primary, acting, acting_primary);
  }
  void pg_to_up_acting_osds(pg_t pg, vector<int>& up, vector<int>& acting) const {
    int up_primary, acting_primary;
    pg_to_up_acting_osds(pg, &up, &up_primary, &acting, &acting_primary);
  }
  bool pg_is_ec(pg_t pg) const {
    auto i = pools.find(pg.pool());
    assert(i != pools.end());
    return i->second.ec_pool();
  }
  bool get_primary_shard(const pg_t& pgid, spg_t *out) const {
    auto i = get_pools().find(pgid.pool());
    if (i == get_pools().end()) {
      return false;
    }
    if (!i->second.ec_pool()) {
      *out = spg_t(pgid);
      return true;
    }
    int primary;
    vector<int> acting;
    pg_to_acting_osds(pgid, &acting, &primary);
    for (uint8_t i = 0; i < acting.size(); ++i) {
      if (acting[i] == primary) {
        *out = spg_t(pgid, shard_id_t(i));
        return true;
      }
    }
    return false;
  }

  int64_t lookup_pg_pool_name(const string& name) const {
    auto p = name_pool.find(name);
    if (p == name_pool.end())
      return -ENOENT;
    return p->second;
  }

  int64_t get_pool_max() const {
    return pool_max;
  }
  const mempool::osdmap::map<int64_t,pg_pool_t>& get_pools() const {
    return pools;
  }
  mempool::osdmap::map<int64_t,pg_pool_t>& get_pools() {
    return pools;
  }
  const string& get_pool_name(int64_t p) const {
    auto i = pool_name.find(p);
    assert(i != pool_name.end());
    return i->second;
  }
  bool have_pg_pool(int64_t p) const {
    return pools.count(p);
  }
  const pg_pool_t* get_pg_pool(int64_t p) const {
    auto i = pools.find(p);
    if (i != pools.end())
      return &i->second;
    return NULL;
  }
  unsigned get_pg_size(pg_t pg) const {
    auto p = pools.find(pg.pool());
    assert(p != pools.end());
    return p->second.get_size();
  }
  int get_pg_type(pg_t pg) const {
    auto p = pools.find(pg.pool());
    assert(p != pools.end());
    return p->second.get_type();
  }


  pg_t raw_pg_to_pg(pg_t pg) const {
    auto p = pools.find(pg.pool());
    assert(p != pools.end());
    return p->second.raw_pg_to_pg(pg);
  }

  // pg -> acting primary osd
  int get_pg_acting_primary(pg_t pg) const {
    int primary = -1;
    _pg_to_up_acting_osds(pg, nullptr, nullptr, nullptr, &primary);
    return primary;
  }

  /*
   * check whether an spg_t maps to a particular osd
   */
  bool is_up_acting_osd_shard(spg_t pg, int osd) const {
    vector<int> up, acting;
    _pg_to_up_acting_osds(pg.pgid, &up, NULL, &acting, NULL, false);
    if (pg.shard == shard_id_t::NO_SHARD) {
      if (calc_pg_role(osd, acting, acting.size()) >= 0 ||
	  calc_pg_role(osd, up, up.size()) >= 0)
	return true;
    } else {
      if (pg.shard < (int)acting.size() && acting[pg.shard] == osd)
	return true;
      if (pg.shard < (int)up.size() && up[pg.shard] == osd)
	return true;
    }
    return false;
  }


  /* what replica # is a given osd? 0 primary, -1 for none. */
  static int calc_pg_rank(int osd, const vector<int>& acting, int nrep=0);
  static int calc_pg_role(int osd, const vector<int>& acting, int nrep=0);
  static bool primary_changed(
    int oldprimary,
    const vector<int> &oldacting,
    int newprimary,
    const vector<int> &newacting);
  
  /* rank is -1 (stray), 0 (primary), 1,2,3,... (replica) */
  int get_pg_acting_rank(pg_t pg, int osd) const {
    vector<int> group;
    int nrep = pg_to_acting_osds(pg, group);
    return calc_pg_rank(osd, group, nrep);
  }
  /* role is -1 (stray), 0 (primary), 1 (replica) */
  int get_pg_acting_role(const pg_t& pg, int osd) const {
    vector<int> group;
    int nrep = pg_to_acting_osds(pg, group);
    return calc_pg_role(osd, group, nrep);
  }

  bool osd_is_valid_op_target(pg_t pg, int osd) const {
    int primary;
    vector<int> group;
    int nrep = pg_to_acting_osds(pg, &group, &primary);
    if (osd == primary)
      return true;
    if (pg_is_ec(pg))
      return false;

    return calc_pg_role(osd, group, nrep) >= 0;
  }

  int clean_pg_upmaps(
    CephContext *cct,
    Incremental *pending_inc);

  bool try_pg_upmap(
    CephContext *cct,
    pg_t pg,                       ///< pg to potentially remap
    const set<int>& overfull,      ///< osds we'd want to evacuate
    const vector<int>& underfull,  ///< osds to move to, in order of preference
    vector<int> *orig,
    vector<int> *out);             ///< resulting alternative mapping

  int calc_pg_upmaps(
    CephContext *cct,
    float max_deviation, ///< max deviation from target (value < 1.0)
    int max_iterations,  ///< max iterations to run
    const set<int64_t>& pools,        ///< [optional] restrict to pool
    Incremental *pending_inc
    );

  /*
   * handy helpers to build simple maps...
   */
  /**
   * Build an OSD map suitable for basic usage. If **num_osd** is >= 0
   * it will be initialized with the specified number of OSDs in a
   * single host. If **num_osd** is < 0 the layout of the OSD map will 
   * be built by reading the content of the configuration file.
   *
   * @param cct [in] in core ceph context 
   * @param e [in] initial epoch
   * @param fsid [in] id of the cluster
   * @param num_osd [in] number of OSDs if >= 0 or read from conf if < 0
   * @return **0** on success, negative errno on error.
   */
  int build_simple(CephContext *cct, epoch_t e, uuid_d &fsid,
		   int num_osd, int pg_bits, int pgp_bits);
  static int _build_crush_types(CrushWrapper& crush);
  static int build_simple_crush_map(CephContext *cct, CrushWrapper& crush,
				    int num_osd, ostream *ss);
  static int build_simple_crush_map_from_conf(CephContext *cct,
					      CrushWrapper& crush,
					      ostream *ss);
  static int build_simple_crush_rulesets(CephContext *cct, CrushWrapper& crush,
					 const string& root,
					 ostream *ss);

  bool crush_ruleset_in_use(int ruleset) const;

  void clear_temp() {
    pg_temp->clear();
    primary_temp->clear();
  }

private:
  void print_osd_line(int cur, ostream *out, Formatter *f) const;
public:
  void print(ostream& out) const;
  void print_pools(ostream& out) const;
  void print_summary(Formatter *f, ostream& out) const;
  void print_oneline_summary(ostream& out) const;
  void print_tree(Formatter *f, ostream *out) const;

  int summarize_mapping_stats(
    OSDMap *newmap,
    const set<int64_t> *pools,
    std::string *out,
    Formatter *f) const;

  string get_flag_string() const;
  static string get_flag_string(unsigned flags);
  static void dump_erasure_code_profiles(
    const mempool::osdmap::map<string,map<string,string> > &profiles,
    Formatter *f);
  void dump(Formatter *f) const;
  static void generate_test_instances(list<OSDMap*>& o);
  bool check_new_blacklist_entries() const { return new_blacklist_entries; }
};
WRITE_CLASS_ENCODER_FEATURES(OSDMap)
WRITE_CLASS_ENCODER_FEATURES(OSDMap::Incremental)

typedef ceph::shared_ptr<const OSDMap> OSDMapRef;

inline ostream& operator<<(ostream& out, const OSDMap& m) {
  m.print_oneline_summary(out);
  return out;
}


#endif
