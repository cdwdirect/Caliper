// Sos.cpp
// Caliper Sos Integration Service

#include "caliper/CaliperService.h"

#include "caliper/Caliper.h"
#include "caliper/SnapshotRecord.h"

#include "caliper/common/Log.h"
#include "caliper/common/RuntimeConfig.h"
#include "caliper/common/SnapshotTextFormatter.h"

#include <atomic>
#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <sstream>
#include <vector>

#include <sos.h>

using namespace cali;
using namespace std;

#define SNAP_MAX 128

namespace
{

static std::atomic<int> snapshot_id { 0 };

const ConfigSet::Entry   configdata[] = {
    { "trigger_attr", CALI_TYPE_STRING, "",
      "Attribute that triggers flush & publish",
      "Attribute that triggers flush & publish"
    },
    ConfigSet::Terminator
};

// Takes an unpacked Caliper snapshot and publishes it in SOS
void pack_snapshot(SOS_pub* sos_pub, bool yn_publish, int snapshot_id, const std::map< Attribute, std::vector<Variant> >& unpacked_snapshot) {
    for (auto &p : unpacked_snapshot) {
        switch (p.first.type()) {
        case CALI_TYPE_STRING:
        {
            std::string pubstr;

            for (const Variant &val : p.second)
                pubstr.append(val.to_string()).append(pubstr.empty() ? "" : "/");

            SOS_pack_related(sos_pub, snapshot_id, p.first.name_c_str(), SOS_VAL_TYPE_STRING, pubstr.c_str());
        }
        break;
        case CALI_TYPE_ADDR:
        case CALI_TYPE_INT:
        case CALI_TYPE_UINT:
        case CALI_TYPE_BOOL:
        {
            int64_t val = p.second.front().to_int();
            SOS_pack_related(sos_pub, snapshot_id, p.first.name_c_str(), SOS_VAL_TYPE_INT, &val);
        }
        break;
        case CALI_TYPE_DOUBLE:
        {
            double val = p.second.front().to_double();
            SOS_pack_related(sos_pub, snapshot_id, p.first.name_c_str(), SOS_VAL_TYPE_DOUBLE, &val);   
        }
        default:
            ;
        }
    }
    if (yn_publish == true) {
        SOS_publish(sos_pub);
    }
    
    return;
}

class SosService
{
    ConfigSet                   config;
    static unique_ptr<SosService> 
                                s_sos;

    SOS_runtime *sos_runtime;
    SOS_pub     *sos_publication_handle;

    Attribute   trigger_attr;
    Attribute   iter_class_attr;

    void flush_and_publish(Caliper* c) {
        Log(2).stream() << "sos: Publishing Caliper data" << std::endl;

        c->flush(nullptr, [this,c](const SnapshotRecord* snapshot){
                pack_snapshot(sos_publication_handle, false, ++snapshot_id, snapshot->unpack(*c));
                return true;
            });
        SOS_publish(sos_publication_handle);
        c->clear(); //Avoids re-publishing snapshots.
    }

    void create_attr(const Attribute& attr) {
        if (attr.name() == config.get("trigger_attr").to_string())
            trigger_attr = attr;        
    }


    void post_begin(Caliper *c, const Attribute& attr) {
        if (trigger_attr != Attribute::invalid && attr.id() == trigger_attr.id()) {
            SnapshotRecord::FixedSnapshotRecord<SNAP_MAX> data;
            SnapshotRecord rec(data);

            c->pull_snapshot(CALI_SCOPE_THREAD | CALI_SCOPE_PROCESS, nullptr, &rec);
        }
    }

    void pre_end(Caliper *c, const Attribute &attr) {
        if (trigger_attr != Attribute::invalid && attr.id() == trigger_attr.id()) {
            SnapshotRecord::FixedSnapshotRecord<SNAP_MAX> data;
            SnapshotRecord rec(data);

            static unsigned int iter = 0;

            // NOTE: Uncomment in case of emergency:
            double pull_before, pull_after;
            double pack_before, pack_after;
            double recu_before, recu_after;
            double publ_before, publ_after;
            double cclr_before, cclr_after;

            SOS_TIME(pull_before);
            c->pull_snapshot(CALI_SCOPE_THREAD | CALI_SCOPE_PROCESS, nullptr, &rec);
            SOS_TIME(pull_after);
            
            SOS_TIME(recu_before);
            auto recunpacked = rec.unpack(*c);
            SOS_TIME(recu_after);

            SOS_TIME(pack_before);
            pack_snapshot(sos_publication_handle, false, ++snapshot_id, recunpacked);
            SOS_TIME(pack_after);
            
            iter++;
            SOS_TIME(publ_before);
            SOS_publish(sos_publication_handle);
            SOS_TIME(publ_after);
            //if (not (iter % ITER_PER_PUBLISH)) {
            //    SOS_publish(sos_publication_handle);
            //} else {
            //    publ_before = 0.0;
            //    publ_after = 0.0;
            //}
            SOS_TIME(cclr_before);
            c->clear(); //Avoids re-publishing snapshots.
            SOS_TIME(cclr_after);

            fprintf(stdout,
                "sos:"
                " c->pull_snapshot: %0.8F,"
                " rec.unpack: %0.8F,"
                " pack_snapshot: %0.8F,"
                " SOS_publish: %0.8F (%d'th time),"
                " c->clear: %0.8F\n",
                pull_after - pull_before,
                recu_after - recu_before,
                pack_after - pack_before,
                publ_after - publ_before, iter,
                cclr_after - cclr_before);
            fflush(stdout);
        }
    }




    // APOLLO-supporting version: This is not how we want to be pulishing/ingesting mass data.
    void process_snapshot(Caliper* c, const SnapshotRecord* trigger_info, const SnapshotRecord* snapshot) {
        pack_snapshot(sos_publication_handle, false, ++snapshot_id, snapshot->unpack(*c));
    }

    // APOLLO-supporting version: DISABLED
    //       (normally...) If it is the end of any iteration, OR the specific client-named attribute, flush.
    void post_end(Caliper* c, const Attribute& attr) {
        //if (   (attr.get(iter_class_attr) == Variant(true))
        //    || (trigger_attr != Attribute::invalid && attr.id() == trigger_attr.id()))
        //    flush_and_publish(c);
    }

    // Initialize the SOS runtime, and create our publication handle
    void post_init(Caliper* c) {
        sos_runtime            = NULL;
        sos_publication_handle = NULL;
        SOS_init(&sos_runtime, SOS_ROLE_CLIENT,
                    SOS_RECEIVES_NO_FEEDBACK, NULL);
        SOS_pub_init(sos_runtime, &sos_publication_handle,
                    "caliper.data", SOS_NATURE_DEFAULT);
        char *freq_env = NULL;
        freq_env = getenv("CALI_SOS_ITER_PER_PUBLISH");
        if (freq_env == NULL) {
            ITER_PER_PUBLISH = 1;
        } else {
            ITER_PER_PUBLISH = atoi(freq_env);
        }
        // NOTE: Check for trigger_attr again at create_attr(...) because it may
        //       not have been expressed to Caliper yet.
        trigger_attr    = c->get_attribute(config.get("trigger_attr").to_string());
        iter_class_attr = c->get_attribute("class.iteration");
    }

    // static callbacks

    static void create_attr_cb(Caliper*, const Attribute& attr) {
        s_sos->create_attr(attr);
    }

    static void process_snapshot_cb(Caliper* c, const SnapshotRecord* trigger_info, const SnapshotRecord* snapshot) {
        std::cout << "sos: Processing snapshot." << std::endl;
        s_sos->process_snapshot(c, trigger_info, snapshot);
    }

    static void post_begin_cb(Caliper *c, const Attribute& attr, const Variant& val) {
        s_sos->post_begin(c, attr);
    }

    static void pre_end_cb(Caliper *c, const Attribute& attr, const Variant& val) {
        s_sos->pre_end(c, attr);
    }

    static void post_end_cb(Caliper* c, const Attribute& attr, const Variant& val) {
        s_sos->post_end(c, attr);
    }

    static void post_init_cb(Caliper* c) { 
        s_sos->post_init(c);
    }

    SosService(Caliper* c)
        : config(RuntimeConfig::init("sos", configdata)),
          trigger_attr(Attribute::invalid)
        {
            
            c->events().create_attr_evt.connect(&SosService::create_attr_cb);
            c->events().post_init_evt.connect(&SosService::post_init_cb);
            // c->events().process_snapshot.connect(&SosService::process_snapshot_cb);
            c->events().post_begin_evt.connect(&SosService::post_begin_cb);
            c->events().pre_end_evt.connect(&SosService::pre_end_cb);

            Log(1).stream() << "Registered SOS service" << std::endl;
        }

public:
    int ITER_PER_PUBLISH;
    static void sos_register(Caliper* c) {
        s_sos.reset(new SosService(c));
    }

}; // SosService

unique_ptr<SosService> SosService::s_sos { nullptr };

} // namespace

namespace cali
{
    CaliperService sos_service = { "sos", ::SosService::sos_register };
} // namespace cali
