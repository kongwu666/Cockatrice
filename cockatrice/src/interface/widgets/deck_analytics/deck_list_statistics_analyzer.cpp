#include "deck_list_statistics_analyzer.h"

#include "deck_list_model.h"
#include "deck_list_statistics_analyzer.h"

#include <QRegularExpression>
#include <libcockatrice/card/database/card_database_manager.h>
#include <libcockatrice/deck_list/deck_list.h>

DeckListStatisticsAnalyzer::DeckListStatisticsAnalyzer(QObject *parent,
                                                       DeckListModel *_model,
                                                       DeckListStatisticsAnalyzerConfig _config)
    : QObject(parent), model(_model), config(_config)
{
    connect(model, &DeckListModel::cardsChanged, this, &DeckListStatisticsAnalyzer::analyze);
}

void DeckListStatisticsAnalyzer::analyze()
{
    clearData();

    QList<const DecklistCardNode *> nodes;

    if (config.includeSideboard) {
        nodes = model->getCardNodes();
    } else {
        nodes = model->getCardNodesForZone(DECK_ZONE_MAIN);
    }

    for (auto node : nodes) {
        CardInfoPtr info = CardDatabaseManager::query()->getCardInfo(node->getName());
        if (!info) {
            continue;
        }

        const int amount = node->getNumber();
        QStringList copiesOfName;
        for (int i = 0; i < amount; i++) {
            copiesOfName.append(node->getName());
        }

        // Convert once
        const int cmc = info->getCmc().toInt();
        QStringList types = info->getMainCardType().split(' ');
        QStringList subtypes = info->getCardType().split('-').last().split(" ");
        QString colors = info->getColors();
        int power = info->getPowTough().split("/").first().toInt();
        int toughness = info->getPowTough().split("/").last().toInt();

        // For each copy of card
        // ---------------- Mana Curve ----------------
        if (config.computeManaCurve) {
            manaCurveMap[cmc] += amount;
        }

        // per-type curve
        for (auto &t : types) {
            manaCurveByType[t][cmc] += amount;
            manaCurveCardsByType[t][cmc] << copiesOfName;
        }

        // Per-subtype curve
        for (auto &st : subtypes) {
            manaCurveBySubtype[st][cmc] += amount;
            manaCurveCardsBySubtype[st][cmc] << copiesOfName;
        }

        // per-color curve
        for (auto &c : colors) {
            manaCurveByColor[c][cmc] += amount;
            manaCurveCardsByColor[c][cmc] << copiesOfName;
        }

        // Power/toughness
        manaCurveByPower[QString::number(power)][cmc] += amount;
        manaCurveCardsByPower[QString::number(power)][cmc] << copiesOfName;
        manaCurveByToughness[QString::number(toughness)][cmc] += amount;
        manaCurveCardsByToughness[QString::number(toughness)][cmc] << copiesOfName;

        // ========== Category Counts ===========
        for (auto &t : types) {
            typeCount[t] += amount;
        }
        for (auto &st : subtypes) {
            subtypeCount[st] += amount;
        }
        for (auto &c : colors) {
            colorCount[c] += amount;
        }
        manaValueCount[cmc] += amount;

        // ---------------- Mana Base ----------------
        if (config.computeManaBase) {
            auto prod = determineManaProduction(info->getText());
            for (auto it = prod.begin(); it != prod.end(); ++it) {
                if (it.value() > 0) {
                    productionPipCount[it.key()] += it.value() * amount;
                    productionCardCount[it.key()] += amount;
                }
                manaBaseMap[it.key()] += it.value() * amount;
            }
        }

        // ---------------- Devotion ----------------
        if (config.computeDevotion) {
            auto devo = countManaSymbols(info->getManaCost());
            for (auto &d : devo) {
                if (d.second > 0) {
                    devotionPipCount[QString(d.first)] += d.second * amount;
                    devotionCardCount[QString(d.first)] += amount;
                }
                manaDevotionMap[d.first] += d.second * amount;
            }
        }
    }

    emit statsUpdated();
}

QHash<QString, int> DeckListStatisticsAnalyzer::determineManaProduction(const QString &rulesText)
{
    QHash<QString, int> manaCounts = {{"W", 0}, {"U", 0}, {"B", 0}, {"R", 0}, {"G", 0}, {"C", 0}};

    const QStringList manaSymbols = {"{W}", "{U}", "{B}", "{R}", "{G}", "{C}"};
    const QRegularExpression nlManaRegex(R"((one|two|three|four|five|six|seven)\s+mana)",
                                         QRegularExpression::CaseInsensitiveOption);

    const QStringList lines = rulesText.split('\n');
    for (const auto &line : lines) {
        const QString addKeyword = "Add ";
        int addPos = line.indexOf(addKeyword, 0, Qt::CaseInsensitive);
        if (addPos == -1) {
            continue;
        }
        QString afterAdd = line.mid(addPos + addKeyword.length());

        // Strategy 1: count {X} mana symbols
        for (const auto &sym : manaSymbols) {
            manaCounts[sym.mid(1, 1)] += afterAdd.count(sym, Qt::CaseInsensitive);
        }

        // Strategy 2: natural language mana (e.g. "Add three mana of any one color")
        QRegularExpressionMatch nlMatch = nlManaRegex.match(afterAdd);
        if (nlMatch.hasMatch()) {
            const QHash<QString, int> wordToNumber = {{"one", 1},  {"two", 2}, {"three", 3}, {"four", 4},
                                                      {"five", 5}, {"six", 6}, {"seven", 7}};
            QString amountStr = nlMatch.captured(1).toLower();
            int amount = wordToNumber.value(amountStr, 0);
            if (amount > 0) {
                for (const auto &color : {QStringLiteral("W"), QStringLiteral("U"), QStringLiteral("B"),
                                          QStringLiteral("R"), QStringLiteral("G")}) {
                    manaCounts[color] += amount;
                }
            }
        }
    }

    return manaCounts;
}

std::unordered_map<char, int> DeckListStatisticsAnalyzer::countManaSymbols(const QString &manaString)
{
    std::unordered_map<char, int> manaCounts = {{'W', 0}, {'U', 0}, {'B', 0}, {'R', 0}, {'G', 0}};

    int len = manaString.length();
    for (int i = 0; i < len; ++i) {
        if (manaString[i] == '{') {
            ++i; // Move past '{'
            if (i < len && manaCounts.find(manaString[i].toLatin1()) != manaCounts.end()) {
                char mana1 = manaString[i].toLatin1();
                ++i; // Move to next character
                if (i < len && manaString[i] == '/') {
                    ++i; // Move past '/'
                    if (i < len && manaCounts.find(manaString[i].toLatin1()) != manaCounts.end()) {
                        char mana2 = manaString[i].toLatin1();
                        manaCounts[mana1]++;
                        manaCounts[mana2]++;
                    } else {
                        // Handle cases like "{W/}" where second part is invalid
                        manaCounts[mana1]++;
                    }
                } else {
                    manaCounts[mana1]++;
                }
            }
            // Ensure we always skip to the closing '}'
            while (i < len && manaString[i] != '}') {
                ++i;
            }
        }
        // Check if the character is a standalone mana symbol (not inside {})
        else if (manaCounts.find(manaString[i].toLatin1()) != manaCounts.end()) {
            manaCounts[manaString[i].toLatin1()]++;
        }
    }

    return manaCounts;
}

// Hypergeometric probability: P(X=k)
double DeckListStatisticsAnalyzer::hypergeometric(int N, int K, int n, int k)
{
    if (k < 0 || k > n || K > N) {
        return 0.0;
    }

    auto choose = [](int n, int r) -> double {
        if (r > n) {
            return 0.0;
        }
        if (r == 0 || r == n) {
            return 1.0;
        }
        double res = 1.0;
        for (int i = 1; i <= r; ++i) {
            res *= (n - r + i);
            res /= i;
        }
        return res;
    };

    return choose(K, k) * choose(N - K, n - k) / choose(N, n);
}

void DeckListStatisticsAnalyzer::clearData()
{
    manaBaseMap.clear();
    manaCurveMap.clear();
    manaDevotionMap.clear();

    devotionPipCount.clear();
    devotionCardCount.clear();

    productionPipCount.clear();
    productionCardCount.clear();

    manaCurveByType.clear();
    manaCurveBySubtype.clear();
    manaCurveByColor.clear();
    manaCurveByPower.clear();
    manaCurveByToughness.clear();

    manaCurveCardsByType.clear();
    manaCurveCardsBySubtype.clear();
    manaCurveCardsByColor.clear();
    manaCurveCardsByPower.clear();
    manaCurveCardsByToughness.clear();

    typeCount.clear();
    subtypeCount.clear();
    colorCount.clear();
    rarityCount.clear();
    manaValueCount.clear();
}