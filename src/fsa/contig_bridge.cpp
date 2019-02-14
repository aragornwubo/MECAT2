#include "contig_bridge.hpp"

#include <algorithm>
#include <sstream>
#include <iostream>

ArgumentParser ContigBridge::GetArgumentParser() {
    ArgumentParser ap("fsa_ctg_bridge", "Bridge contigs", "1.0");
    ap.AddNamedOption(ctg2ctg_file_, "ctg2ctg_file", "filename containing overlaps between contigs");
    ap.AddNamedOption(read_min_length_, "read_min_length", "minimum rawread length");
    ap.AddNamedOption(ctg_min_length_, "ctg_min_length", "minimum contig length");
    ap.AddNamedOption(read2ctg_min_identity_, "read2ctg_min_identity", "minimum identity of overlaps between rawreads and contigs");
    ap.AddNamedOption(ctg2ctg_min_identity_, "ctg2ctg_min_identity", "minimum identity of overlaps between contigs");
    ap.AddNamedOption(read2ctg_max_overhang_, "read2ctg_max_overhang", "maximum overhang of overlaps between rawreads and contigs");
    ap.AddNamedOption(ctg2ctg_max_overhang_, "ctg2ctg_max_overhang", "maximum overhang of overlaps between contigs");
    ap.AddNamedOption(read2ctg_min_aligned_length_, "read2ctg_min_aligned_length", "minimum aligned length of overlaps between rawreads and contigs");
    ap.AddNamedOption(ctg2ctg_min_aligned_length_, "ctg2ctg_min_aligned_length", "minimum aligned length of overlaps between contigs");
    ap.AddNamedOption(read2ctg_min_coverage_, "read2ctg_min_coverage", "minimum coverage of links between rawreads and contigs");
    ap.AddNamedOption(min_contig_length_, "min_contig_length", "minimum length of bridged contig");
    ap.AddNamedOption(output_directory_, "output_directory", "directory for output files");   
    ap.AddNamedOption(select_branch_, "select_branch", "selecting method when encountering branches in the graph, \"no\" = do not select any branch, \"best\" = select the most probable branch", "\"no|best\"");

    ap.AddNamedOption(dump_, "dump", "for testing, dump intermediate files");
    ap.AddNamedOption(thread_size_, "thread_size", "number of threads");

    ap.AddPositionOption(read_file_, "rawreads", "rawread file");
    ap.AddPositionOption(contig_file_, "contigs", "contig file");
    ap.AddPositionOption(read2ctg_file_, "read2ctg", "file containing overlaps between rawread and contigs");
    ap.AddPositionOption(bridged_contig_file_, "bridged_contigs", "output file");

    return ap;
}

bool ContigBridge::ParseArgument(int argc, const char *const argv[]) {
    return GetArgumentParser().ParseArgument(argc, argv);
}

void ContigBridge::Usage() {
    std::cout << GetArgumentParser().Usage();
}

void ContigBridge::Run() {
    PrintArguments();

    if (read2ctg_min_identity_ < 0 || read2ctg_max_overhang_ < 0) {
        LOG(INFO)("Auto select read2ctg parameters");
        AutoSelectRead2ctgParams();
    }

    if (!ctg2ctg_file_.empty() && (ctg2ctg_min_identity_ < 0 || ctg2ctg_max_overhang_ < 0)) {
        LOG(INFO)("Auto select ctg2ctg parameters");
        AutoSelectCtg2ctgParams();
    }

    contig_links_.SetParameter("read2ctg_min_identity", read2ctg_min_identity_);
    contig_links_.SetParameter("ctg2ctg_min_identity", ctg2ctg_min_identity_);
    contig_links_.SetParameter("read_min_length", read_min_length_);
    contig_links_.SetParameter("ctg_min_length", ctg_min_length_);
    contig_links_.SetParameter("read2ctg_max_overhang", read2ctg_max_overhang_);
    contig_links_.SetParameter("ctg2ctg_max_overhang", ctg2ctg_max_overhang_);
    contig_links_.SetParameter("read2ctg_min_aligned_length", read2ctg_min_aligned_length_);
    contig_links_.SetParameter("ctg2ctg_min_aligned_length", ctg2ctg_min_aligned_length_);
    contig_links_.SetParameter("read2ctg_min_coverage", read2ctg_min_coverage_);
    contig_links_.SetParameter("thread_size", thread_size_);


    if (!ctg2ctg_file_.empty()) {
        LOG(INFO)("Load ctg2ctg file %s", ctg2ctg_file_.c_str());
        contig_links_.LoadC2cFile(ctg2ctg_file_);
    }

    LOG(INFO)("Load read2ctg file %s", read2ctg_file_.c_str());
    contig_links_.LoadR2cFile(read2ctg_file_);


    LOG(INFO)("Selecting best link");
    contig_links_.AnalyzeSupport();
   
    LOG(INFO)("Create graph and identify best path");
    contig_graph_.Create();
    contig_graph_.CalucateBest("support");
    contig_graph_.IdentifyPaths(select_branch_);


    LOG(INFO)("Load read file %s", read_file_.c_str());
    read_store_.Load(read_file_, "", 4);   
    LOG(INFO)("Load contig file %s", contig_file_.c_str());
    read_store_.Load(contig_file_);
    contigs_ = read_store_.IdsInFile(contig_file_);

    LOG(INFO)("Save bridged contigs to %s", bridged_contig_file_.c_str());
    SaveBridgedContigs(bridged_contig_file_);

    if (dump_) {
        LOG(INFO)("Dump internal variables");
        Dump();    
    }
    
    LOG(INFO)("END");
}

void ContigBridge::SaveBridgedContigs(const std::string &fname) {
    std::vector<std::array<size_t, 3>> all_contigs;
    std::vector<const std::deque<ContigNode*>*> bridged_contigs;
    std::unordered_set<Seq::Id> bridged_contig_ids;

    for (auto &path : contig_graph_.GetPaths()) {
        if (path.size() >= 2) {
            bridged_contigs.push_back(&path);
            size_t length = read_store_.GetSeqLength(Seq::EndIdToId(path[0]->Id()));
            bridged_contig_ids.insert(Seq::EndIdToId(path[0]->Id())) ;
            for (size_t i = 1; i<path.size(); ++i) {
                length += contig_graph_.GetEdge(path[i-1], path[i])->LinkLength();
                length -= read_store_.GetSeqLength(Seq::EndIdToId(path[i-1]->Id()));

                bridged_contig_ids.insert(Seq::EndIdToId(path[i]->Id()));
            }

            all_contigs.push_back(std::array<size_t, 3>{1, bridged_contigs.size()-1, length});
        }
    }

    // Add that contigs that are not found in the graph
    for (auto c : contigs_) {
        if (bridged_contig_ids.find(c) == bridged_contig_ids.end() &&
            contig_graph_.GetContained().find(c) == contig_graph_.GetContained().end()) {
            all_contigs.push_back(std::array<size_t, 3>{0, (size_t)c, read_store_.GetSeqLength(c)});
        }
    }

    std::sort(all_contigs.begin(), all_contigs.end(), [](const std::array<size_t, 3> &a, const std::array<size_t, 3> &b) {
        return a[2] > b[2]; });


    std::ofstream of(fname);
    if (of.is_open()) {
        for (auto &c : all_contigs) {

            std::string head; 
            std::string seq;

            if (c[0] == 0) {
                head = read_store_.IdToName(c[1]);
                seq = read_store_.GetSeq(c[1]);
            }
            else {
                assert(c[0] == 1);
                auto &path = *bridged_contigs[c[1]];
                assert(path.size() >= 2);

                head = read_store_.IdToName(Seq::EndIdToId(path[0]->Id()));
                seq = read_store_.GetSeq(Seq::EndIdToId(path[0]->Id()));
                if (path[0]->Id() > 0) seq = Seq::ReverseComplement(seq);

                for (size_t i = 1; i < path.size(); ++i) {
                    auto e = contig_graph_.GetEdge(path[i - 1], path[i]);

                    auto seq_area = e->GetSeqArea();
                    for (auto sa : seq_area) {
                        head += "_" + read_store_.IdToName(sa.id);
                        seq += read_store_.GetSeq(sa);
                    }

                }
                
            }
            if ((int)seq.length() >= min_contig_length_) {
                of << ">" << head << "\n" << seq << "\n";
            }
        }

    }
    else {
        LOG(ERROR)("Failed to Write BridgeContigs file: %s", fname.c_str());
    }
}

void ContigBridge::PrintArguments() const {

    LOG(INFO)("Arguments: \n%s", ap_.PrintOptions().c_str());

}

void ContigBridge::AutoSelectCtg2ctgParams() {
 
    assert(!ctg2ctg_file_.empty());
    std::unordered_map<Seq::Id, ContigBridge::ReadStatInfo> readInfos = StatReadInfo(ctg2ctg_file_,95,250);

    if (ctg2ctg_min_identity_ < 0) {
        AutoSelectCtg2ctgMinIdentity(readInfos);
    }

    if (ctg2ctg_max_overhang_ < 0) {
        AutoSelectCtg2ctgMaxOverhang(readInfos);
    }
}


void ContigBridge::AutoSelectCtg2ctgMinIdentity(const std::unordered_map<Seq::Id, ReadStatInfo> &readInfos) {
    std::vector<std::array<double,2>> idents(readInfos.size());
    std::transform(readInfos.begin(), readInfos.end(), idents.begin(), [&](const std::pair<int,ReadStatInfo> &kv) {
        return std::array<double,2>{kv.second.identity,(double)kv.second.score/1000}; // x/1000 is to prevent overflow
    });
    double median = 0;
    double mad = 0;
    ComputeMedianAbsoluteDeviation(idents, median, mad);
    ctg2ctg_min_identity_ = median - 6*1.4826*mad;
    LOG(INFO)("Auto Select ctg2ctg_min_identity = %.02f, median=%.02f, mad=%.02f", ctg2ctg_min_identity_, median, mad);
}

void ContigBridge::AutoSelectCtg2ctgMaxOverhang(const std::unordered_map<Seq::Id, ReadStatInfo> &readInfos) {
    std::vector<std::array<double,2>> overhangs(readInfos.size());
    std::transform(readInfos.begin(), readInfos.end(), overhangs.begin(), [&](const std::pair<int,ReadStatInfo> &kv) {
        return std::array<double,2>{kv.second.overhang*1.0, kv.second.len/100.0};
    });

    double median = 0;
    double mad = 0;
    ComputeMedianAbsoluteDeviation(overhangs, median, mad);
    ctg2ctg_max_overhang_ = (int)(median + 6*1.4826*mad);
    LOG(INFO)("Auto Select ctg2ctg_max_overhang = %d, median=%f, mad=%f", ctg2ctg_max_overhang_, median, mad);

}

void ContigBridge::AutoSelectRead2ctgParams() {
 
    std::unordered_map<Seq::Id, ContigBridge::ReadStatInfo> readInfos = StatReadInfo(read2ctg_file_, 75, 500);
    // for debug, print stat info
    //for (auto o : readInfos) {
    //    printf("overhang %d %d\n", o.first, o.second.overhang);
    //}

    //if (min_length_ < 0 || (genome_size_ > 0 && coverage_ > 0)) {
    //    AutoSelectMinLength(readInfos);
    //}

    if (read2ctg_min_identity_ < 0) {
        AutoSelectRead2ctgMinIdentity(readInfos);
    }

    if (read2ctg_max_overhang_ < 0) {
        AutoSelectRead2ctgMaxOverhang(readInfos);
    }

    //if (min_aligned_length_ < 0) {
    //    AutoSelectMinAlignedLength(readInfos);
    //}
}


void ContigBridge::AutoSelectRead2ctgMinIdentity(const std::unordered_map<Seq::Id, ReadStatInfo> &readInfos) {
    std::vector<std::array<double,2>> idents(readInfos.size());
    std::transform(readInfos.begin(), readInfos.end(), idents.begin(), [&](const std::pair<int,ReadStatInfo> &kv) {
        return std::array<double,2>{kv.second.identity,(double)kv.second.score/1000}; // x/1000 is to prevent overflow
    });
    double median = 0;
    double mad = 0;
    ComputeMedianAbsoluteDeviation(idents, median, mad);
    read2ctg_min_identity_ = median - 3*1.4826*mad;
    LOG(INFO)("Auto Select read2ctg_min_identity = %.02f, median=%.02f, mad=%.02f", read2ctg_min_identity_, median, mad);
}

void ContigBridge::AutoSelectRead2ctgMaxOverhang(const std::unordered_map<Seq::Id, ReadStatInfo> &readInfos) {
    std::vector<std::array<double,2>> overhangs(readInfos.size());
    std::transform(readInfos.begin(), readInfos.end(), overhangs.begin(), [&](const std::pair<int,ReadStatInfo> &kv) {
        return std::array<double,2>{kv.second.overhang*1.0, kv.second.score/100.0};
    });

    double median = 0;
    double mad = 0;
    ComputeMedianAbsoluteDeviation(overhangs, median, mad);
    read2ctg_max_overhang_ = (int)(median + 3*1.4826*mad);
    LOG(INFO)("Auto Select read2ctg_max_overhang = %d, median=%f, mad=%f", read2ctg_max_overhang_, median, mad);

}

std::unordered_map<Seq::Id, ContigBridge::ReadStatInfo> ContigBridge::StatReadInfo(const std::string &fname, int th_identity, int th_overhang) {
   std::mutex mutex;

    size_t block_size = 50000;

    std::unordered_map<int, ReadStatInfo> readInfos;
    
    struct WorkArea{
        std::unordered_map<int, ReadStatInfo> readInfos;
        void Clear() { readInfos.clear(); }
    };
    

    std::list<WorkArea> works;  // for each thread. Vector may cause memory reallocating, so list is used.

    auto alloc_work = [&]() -> WorkArea& {
        std::lock_guard<std::mutex> lock(mutex);
        works.push_back(WorkArea());
        return works.back();
    };

    auto combine = [&](WorkArea& work) {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto &i : work.readInfos) {
            auto iter = readInfos.find(i.first); 
            if (iter != readInfos.end()) {
                if (iter->second.score < i.second.score) {
                    iter->second.score = i.second.score;
                    iter->second.identity = i.second.identity;
                }
                assert(iter->second.len >= i.second.len);

                iter->second.count += i.second.count;
                iter->second.aligned += i.second.aligned;
                if (i.second.overhang >= 0) {
                    if (iter->second.overhang < i.second.overhang) iter->second.overhang = i.second.overhang;
                    iter->second.oh_count += i.second.oh_count;
                }
    
            } else {
                readInfos[i.first] = i.second;

            }
        }

        work.Clear();
    };

    auto scan_overlap = [&](Overlap& o) {
        WorkArea thread_local &work = alloc_work();
        auto loc = o.Location(th_overhang);
        if (o.identity_ > th_identity && o.AlignedLength() >= 2000 && 
            loc != Overlap::Loc::Abnormal) {

            auto overhang = o.Overhang();
            int score = o.identity_*o.AlignedLength();

            auto aread = work.readInfos.find(o.a_.id);
            auto bread = work.readInfos.find(o.b_.id);

            if (aread != work.readInfos.end()) {
                if (aread->second.score < score) {
                    aread->second.score = score;
                    aread->second.identity = o.identity_;
                }
                assert(aread->second.len == o.a_.len);
                if (overhang[0] >= 0) {
                    if (overhang[0] > aread->second.overhang)  aread->second.overhang = overhang[0];   
                    aread->second.oh_count += 1;
                }
                aread->second.aligned += o.a_.end - o.a_.start;
                aread->second.count += 1;

            } else {
                ReadStatInfo info;
                info.identity = o.identity_;
                if (overhang[0] >= 0) {
                    info.overhang = overhang[0];
                    info.oh_count = 1;
                }
                info.score = score;
                info.len = o.a_.len;
                info.aligned = o.a_.end - o.a_.start;
                info.count = 1;
                work.readInfos[o.a_.id] = info;
            }

            if (bread != work.readInfos.end()) {
                if (bread->second.score < score) {
                    bread->second.score = score;
                    bread->second.identity = o.identity_;
                }
                assert(bread->second.len == o.b_.len);
                if (overhang[1] >= 0) {
                    if (overhang[1] > bread->second.overhang)  bread->second.overhang = overhang[1];   
                    bread->second.oh_count += 1;
                }
                bread->second.aligned += o.b_.end - o.b_.start;
                bread->second.count += 1;
            } else {
                ReadStatInfo info;
                info.identity = o.identity_;
                if (overhang[1] >= 0) {
                    info.overhang = overhang[1];
                    info.oh_count = 1;
                }
                info.score = score;
                info.len = o.b_.len;
                info.aligned = o.b_.end - o.b_.start;
                info.count = 1;
                work.readInfos[o.b_.id] = info;
            }

        }
        if (work.readInfos.size() >= block_size) {
            combine(work);
        }
        return false;   // not load to memory
    };

    OverlapStore ol;
    ol.Load(fname, "", thread_size_, scan_overlap);
    for (auto & w : works) {
        combine(w);
    }

    return readInfos;
}

void ContigBridge::Dump()  {
    read_store_.SaveIdToName(OutputPath("id2name.txt"));
    contig_graph_.Output(OutputPath("contig_graph.csv"));
    contig_links_.Dump(OutputPath("links.txt"));
}
