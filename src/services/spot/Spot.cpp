// Copyright (c) 2016, Lawrence Livermore National Security, LLC.  
// Produced at the Lawrence Livermore National Laboratory.
//
// This file is part of Caliper.
// Written by David Boehme, boehme3@llnl.gov.
// LLNL-CODE-678900
// All rights reserved.
//
// For details, see https://github.com/scalability-llnl/Caliper.
// Please also see the LICENSE file for our additional BSD notice.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
//  * Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the disclaimer below.
//  * Redistributions in binary form must reproduce the above copyright notice, this list of
//    conditions and the disclaimer (as noted below) in the documentation and/or other materials
//    provided with the distribution.
//  * Neither the name of the LLNS/LLNL nor the names of its contributors may be used to endorse
//    or promote products derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS
// OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// LAWRENCE LIVERMORE NATIONAL SECURITY, LLC, THE U.S. DEPARTMENT OF ENERGY OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// Spot.cpp
// Outputs Caliper performance results to Spot-parseable formats


#include "caliper/CaliperService.h"

#include "caliper/Caliper.h"
#include "caliper/SnapshotRecord.h"

#include "caliper/reader/Aggregator.h"
#include "caliper/reader/CalQLParser.h"
#include "caliper/reader/QueryProcessor.h"

#include "caliper/common/Log.h"
#include "caliper/common/Node.h"
#include "caliper/common/OutputStream.h"
#include "caliper/common/RuntimeConfig.h"
#include "caliper/common/util/split.hpp"

#include "rapidjson/rapidjson.h"
#include "rapidjson/document.h"
#include "rapidjson/ostreamwrapper.h"
#include "rapidjson/writer.h"

#include <iostream>
#include <sstream>
#include <iterator>
#include <fstream>
using namespace cali;

namespace
{

    class Spot {
        static std::unique_ptr<Spot> s_instance;
        static const ConfigSet::Entry  s_configdata[];
        static int divisor;
        template<typename T>
        using List = std::vector<T>;
        using AggregationHandler = cali::Aggregator*;
        using TimeType = unsigned int;
        using SingleJsonEntryType = List<std::pair<std::string,TimeType>>;
        using JsonListType = List<SingleJsonEntryType>;
        using AggregationDescriptor = std::pair<std::string, std::string>;
        using AggregationDescriptorList = List<AggregationDescriptor>;
        static List<AggregationHandler> m_queries;
        static AggregationDescriptorList m_annotations_and_places;
        static JsonListType m_jsons;
        void process_snapshot(Caliper* c, const SnapshotRecord* snapshot) {
            for(auto& m_query : m_queries){
              m_query->add(*c, snapshot->to_entrylist());
            }
        }

        void flush(Caliper* c, const SnapshotRecord*) {
          for(int i =0 ;i<m_queries.size();i++) {
            auto& m_query = m_queries[i];
            auto& m_json = m_jsons[i];
            std::string grouping = m_annotations_and_places[i].first;
            std::vector<std::string> metrics_of_interest { "time.inclusive.duration", grouping };
            m_query->flush(*c,[&](CaliperMetadataAccessInterface& db,const EntryList& list) {
                std::string name;
                TimeType value;
                for(const auto& entry: list) {
                  for(std::string& attribute_key : metrics_of_interest) {
                       Attribute attr = db.get_attribute(attribute_key);
                       Variant value_iter = entry.value(attr);
                       if(!value_iter.empty()){
                          if(attribute_key == grouping) {
                            name = value_iter.to_string(); 
                          }
                          else {
                            value = value_iter.to_uint();
                          }
                       }
                   }
                }
                m_json.push_back(std::make_pair(name,value));
            });
          }
          for(int i =0 ;i<m_jsons.size();i++) {
            auto place = m_annotations_and_places[i].second;
            auto json = m_jsons[i];
            std::ifstream ifs(place);
            std::string str(std::istreambuf_iterator<char>{ifs}, {});
            rapidjson::Document doc;
            if(str.size() > 0){
              doc.Parse(str.c_str());
              auto& json_series_values = doc["series"];
              for(auto datum : json){
                 std::string series_name = datum.first;
                 for(auto& existing_series_name : json_series_values.GetArray()){
                    if(series_name == existing_series_name.GetString()){
                      auto series_data = doc[series_name.c_str()].GetArray();
                      rapidjson::Value arrarr;
                      arrarr.SetArray();
                      arrarr.PushBack(0,doc.GetAllocator());
                      arrarr.PushBack(((float)datum.second)/(1.0*divisor),doc.GetAllocator());
                      series_data.PushBack(arrarr,doc.GetAllocator());
                    } 
                 }
                 TimeType value = datum.second;
              } 
            }
            else{
              doc.StartObject();
            }
            std::ofstream ofs(place.c_str());
            rapidjson::OStreamWrapper osw(ofs);
            rapidjson::Writer<rapidjson::OStreamWrapper> writer(osw);
            doc.Accept(writer);
          }
        }
        // TODO: reimplement
        Spot()
            { }

        //
        // --- callback functions
        //
        static AggregationHandler create_query_processor(std::string query){
           CalQLParser parser(query.c_str()); 
           if (parser.error()) {
               Log(0).stream() << "spot: config parse error: " << parser.error_msg() << std::endl;
               return nullptr;
           }
           QuerySpec    spec(parser.spec());
           return new Aggregator(spec);
        }
        static std::string query_for_annotation(std::string grouping, std::string metric = "time.inclusive.duration"){
          std::string group_comma_metric = " " +grouping+","+metric+" ";
          return "SELECT " + grouping+",sum("+metric+") " + "WHERE" + group_comma_metric + "GROUP BY " + grouping;
        }
        static void pre_write_cb(Caliper* c, const SnapshotRecord* flush_info) {
            ConfigSet    config(RuntimeConfig::init("spot", s_configdata));
            const std::string&  config_string = config.get("config").to_string().c_str();
            divisor = config.get("time_divisor").to_int();
            std::vector<std::string> logging_configurations;
            util::split(config_string,',',std::back_inserter(logging_configurations));
            for(const auto log_config : logging_configurations){
              m_jsons.emplace_back(std::vector<std::pair<std::string,TimeType>>());
              std::vector<std::string> annotation_and_place;
              util::split(log_config,':',std::back_inserter(annotation_and_place));
              std::string& annotation = annotation_and_place[0];
              std::string& place = annotation_and_place[1];
              std::string query = query_for_annotation(annotation);
              Log(0).stream() << "Spot: establishing query \"" <<query<<'"'<<std::endl;
              m_queries.emplace_back(create_query_processor(query));
              m_annotations_and_places.push_back(std::make_pair(annotation,place));
            }
            //CalQLParser  parser(config.get("config").to_string().c_str());

            //if (parser.error()) {
            //    Log(0).stream() << "spot: config parse error: " << parser.error_msg() << std::endl;
            //    return;
            //}

            //QuerySpec    spec(parser.spec());

            //// set format default to table if it hasn't been set in the query config
            //if (spec.format.opt == QuerySpec::FormatSpec::Default)
            //    spec.format = CalQLParser("format table").spec().format;
            //    
            //OutputStream stream;

            //stream.set_stream(OutputStream::StdOut);

            //std::string filename = config.get("filename").to_string();

            //if (!filename.empty())
            //    stream.set_filename(filename.c_str(), *c, flush_info->to_entrylist());
            //
            s_instance.reset(new Spot());
        }

        static void write_snapshot_cb(Caliper* c, const SnapshotRecord*, const SnapshotRecord* snapshot) {
            if (!s_instance)
                return;

            s_instance->process_snapshot(c, snapshot);
        }

        static void post_write_cb(Caliper* c, const SnapshotRecord* flush_info) {
            if (!s_instance)
                return;

            s_instance->flush(c, flush_info);
        }

    public:

        ~Spot()
            { }

        static void create(Caliper* c) {
            c->events().pre_write_evt.connect(pre_write_cb);
            c->events().write_snapshot.connect(write_snapshot_cb);
            c->events().post_write_evt.connect(post_write_cb);

            Log(1).stream() << "Registered Spot service" << std::endl;
        }
    };

    std::unique_ptr<Spot> Spot::s_instance { nullptr };
    int Spot::divisor = 1000000;
    Spot::List<Spot::AggregationHandler> Spot::m_queries;
    Spot::JsonListType Spot::m_jsons;
    Spot::AggregationDescriptorList Spot::m_annotations_and_places; 
    const ConfigSet::Entry  Spot::s_configdata[] = {
        { "config", CALI_TYPE_STRING, "function:default.json",
          "Attribute:Filename pairs in which to dump Spot data",
          "Attribute:Filename pairs in which to dump Spot data\n"
          "Example: function:testname.json,physics_package:packages.json"
          "   stderr: Standard error stream,\n"
          " or a file name.\n"
        },
        { "recorded_time", CALI_TYPE_STRING, "",
          "Time to use for this version of the code",
          "Time to use for this version of the code"
        },
        { "code_version", CALI_TYPE_STRING, "",
          "Version number (or git hash) to represent this run of the code",
          "Version number (or git hash) to represent this run of the code"
        },
        { "time_divisor", CALI_TYPE_INT, "1000000",
          "Caliper records time in microseconds, this is what we divide by to get time in your units",
          "Caliper records time in microseconds, this is what we divide by to get time in your units. 1000 if you record in milliseconds, 1000000 if seconds"
        },
        ConfigSet::Terminator
    };

} // namespace 

namespace cali
{
    CaliperService spot_service { "spot", ::Spot::create };
}
