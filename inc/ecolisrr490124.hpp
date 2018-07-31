#ifndef E_COLI_SRR490124_HPP
#define E_COLI_SRR490124_HPP

#include <utility>

namespace ecoli_srr490124{
    //coverage is normalized to number of reads in msa
    bool shouldCorrect(double min_col_support, double min_col_coverage,
        double max_col_support, double max_col_coverage,
        double mean_col_support, double mean_col_coverage,
        double median_col_support, double median_col_coverage,
        double maxgini);

    std::pair<int, int> shouldCorrect_forest(double min_col_support, double min_col_coverage,
                double max_col_support, double max_col_coverage,
                double mean_col_support, double mean_col_coverage,
                double median_col_support, double median_col_coverage,
                double maxgini);
}

#endif
