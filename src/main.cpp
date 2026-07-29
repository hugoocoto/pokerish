#include "phevaluator/card.h"
#include "phevaluator/phevaluator.h"

#include <iostream>
#include <istream>

int
main(int argc, char **argv)
{
        phevaluator::Card card = phevaluator::Card("9c");
        phevaluator::Rank rank1;
        rank1 = phevaluator::EvaluateCards(card, "4c", "4s", "Qc", "6c");
        std::cout << "describeCategory:" << rank1.describeCategory() << std::endl;
        std::cout << "describeRank:" << rank1.describeRank() << std::endl;
        std::cout << "describeSampleHand:" << rank1.describeSampleHand() << std::endl;

        rank1 = phevaluator::EvaluateCards("9c", "4c", "4s", "Qc", "6c", "4h");
        std::cout << "describeCategory:" << rank1.describeCategory() << std::endl;
        std::cout << "describeRank:" << rank1.describeRank() << std::endl;
        std::cout << "describeSampleHand:" << rank1.describeSampleHand() << std::endl;

        rank1 = phevaluator::EvaluateCards("9c", "4c", "4s", "Qc", "6c", "4h", "9d");
        std::cout << "describeCategory:" << rank1.describeCategory() << std::endl;
        std::cout << "describeRank:" << rank1.describeRank() << std::endl;
        std::cout << "describeSampleHand:" << rank1.describeSampleHand() << std::endl;

        return 0;
}
