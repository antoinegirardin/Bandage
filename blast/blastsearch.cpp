//Copyright 2015 Ryan Wick

//This file is part of Bandage

//Bandage is free software: you can redistribute it and/or modify
//it under the terms of the GNU General Public License as published by
//the Free Software Foundation, either version 3 of the License, or
//(at your option) any later version.

//Bandage is distributed in the hope that it will be useful,
//but WITHOUT ANY WARRANTY; without even the implied warranty of
//MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//GNU General Public License for more details.

//You should have received a copy of the GNU General Public License
//along with Bandage.  If not, see <http://www.gnu.org/licenses/>.


#include "blastsearch.h"
#include "../graph/assemblygraph.h"
#include <QDir>
#include <QRegularExpression>
#include "buildblastdatabaseworker.h"
#include "runblastsearchworker.h"
#include "../program/settings.h"
#include <QApplication>
#include "../graph/debruijnnode.h"

BlastSearch::BlastSearch() :
    m_blastQueries()
{
}

BlastSearch::~BlastSearch()
{
    cleanUp();
}

void BlastSearch::clearBlastHits()
{
    m_hits.clear();
    m_blastQueries.clearSearchResults();
    m_blastOutput = "";
}

void BlastSearch::cleanUp()
{
    clearBlastHits();
    m_blastQueries.clearAllQueries();
    emptyTempDirectory();
}

//This function uses the contents of m_blastOutput to construct
//the BlastHit objects.
void BlastSearch::buildHitsFromBlastOutput()
{
    QStringList blastHitList = m_blastOutput.split("\n", QString::SkipEmptyParts);

    for (int i = 0; i < blastHitList.size(); ++i)
    {
        QString hit = blastHitList[i];
        QStringList alignmentParts = hit.split('\t');

        if (alignmentParts.size() < 12)
            return;

        QString queryName = alignmentParts[0];
        QString nodeLabel = alignmentParts[1];
        double percentIdentity = alignmentParts[2].toDouble();
        int alignmentLength = alignmentParts[3].toInt();
        int numberMismatches = alignmentParts[4].toInt();
        int numberGapOpens = alignmentParts[5].toInt();
        int queryStart = alignmentParts[6].toInt();
        int queryEnd = alignmentParts[7].toInt();
        int nodeStart = alignmentParts[8].toInt();
        int nodeEnd = alignmentParts[9].toInt();
        double eValue = alignmentParts[10].toDouble();
        int bitScore = alignmentParts[11].toInt();

        //Only save BLAST hits that are on forward strands.
        if (nodeStart > nodeEnd)
            continue;

        QString nodeName = getNodeNameFromString(nodeLabel);
        DeBruijnNode * node;
        if (g_assemblyGraph->m_deBruijnGraphNodes.contains(nodeName))
            node = g_assemblyGraph->m_deBruijnGraphNodes[nodeName];
        else
            return;

        BlastQuery * query = g_blastSearch->m_blastQueries.getQueryFromName(queryName);
        if (query == 0)
            return;

        g_blastSearch->m_hits.push_back(BlastHit(query, node, percentIdentity, alignmentLength,
                                                 numberMismatches, numberGapOpens, queryStart, queryEnd,
                                                 nodeStart, nodeEnd, eValue, bitScore));

        ++(query->m_hits);
    }
}



QString BlastSearch::getNodeNameFromString(QString nodeString)
{
    QStringList nodeStringParts = nodeString.split("_");
    return nodeStringParts[1];
}



bool BlastSearch::findProgram(QString programName, QString * command)
{
    QString findCommand = "which " + programName;
#ifdef Q_OS_WIN32
    findCommand = "WHERE " + programName;
#endif

    QProcess find;

    //On Mac, it's necessary to do some stuff with the PATH variable in order
    //for which to work.
#ifdef Q_OS_MAC
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QStringList envlist = env.toStringList();

    //Add some paths to the process environment
    envlist.replaceInStrings(QRegularExpression("^(?i)PATH=(.*)"), "PATH="
                                                                   "/usr/bin:"
                                                                   "/bin:"
                                                                   "/usr/sbin:"
                                                                   "/sbin:"
                                                                   "/opt/local/bin:"
                                                                   "/usr/local/bin:"
                                                                   "$HOME/bin:"
                                                                   "/usr/local/ncbi/blast/bin:"
                                                                   "\\1");

    find.setEnvironment(envlist);
#endif

    find.start(findCommand);
    find.waitForFinished();

    //On a Mac, we need to use the full path to the program.
#ifdef Q_OS_MAC
    *command = QString(find.readAll()).simplified();
#endif

    return (find.exitCode() == 0);
}





void BlastSearch::clearSomeQueries(std::vector<BlastQuery *> queriesToRemove)
{
    //Remove any hits that are for queries that will be deleted.
    std::vector<BlastHit>::iterator i;
    for (i = m_hits.begin(); i != m_hits.end(); )
    {
        BlastQuery * hitQuery = i->m_query;
        bool hitIsForDeletedQuery = (std::find(queriesToRemove.begin(), queriesToRemove.end(), hitQuery) != queriesToRemove.end());
        if (hitIsForDeletedQuery)
            i = m_hits.erase(i);
        else
            ++i;
    }

    //Now actually delete the queries.
    m_blastQueries.clearSomeQueries(queriesToRemove);
}



void BlastSearch::emptyTempDirectory()
{
    QDir tempDirectory(m_tempDirectory);
    tempDirectory.setNameFilters(QStringList() << "*.*");
    tempDirectory.setFilter(QDir::Files);
    foreach(QString dirFile, tempDirectory.entryList())
        tempDirectory.remove(dirFile);
}


//This function carries out the entire BLAST search procedure automatically, without user input.
//It returns an error string which is empty if all goes well.
QString BlastSearch::doAutoBlastSearch()
{
    cleanUp();

    QString makeblastdbCommand = "makeblastdb";
    if (!findProgram("makeblastdb", &makeblastdbCommand))
        return "Error: The program makeblastdb was not found.  Please install NCBI BLAST to use this feature.";

    BuildBlastDatabaseWorker buildBlastDatabaseWorker(makeblastdbCommand);
    buildBlastDatabaseWorker.buildBlastDatabase();
    if (buildBlastDatabaseWorker.m_error != "")
        return buildBlastDatabaseWorker.m_error;

    loadBlastQueriesFromFastaFile(g_settings->blastQueryFilename);

    QString blastnCommand = "blastn";
    if (!findProgram("blastn", &blastnCommand))
        return "Error: The program blastn was not found.  Please install NCBI BLAST to use this feature.";
    QString tblastnCommand = "tblastn";
    if (!findProgram("tblastn", &tblastnCommand))
        return "Error: The program tblastn was not found.  Please install NCBI BLAST to use this feature.";

    RunBlastSearchWorker runBlastSearchWorker(blastnCommand, tblastnCommand, g_settings->blastSearchParameters);;
    runBlastSearchWorker.runBlastSearch();
    if (runBlastSearchWorker.m_error != "")
        return runBlastSearchWorker.m_error;

    blastTargetChanged("all");

    return "";
}


void BlastSearch::loadBlastQueriesFromFastaFile(QString fullFileName)
{
    std::vector<QString> queryNames;
    std::vector<QString> querySequences;
    readFastaFile(fullFileName, &queryNames, &querySequences);

    for (size_t i = 0; i < queryNames.size(); ++i)
    {
        QApplication::processEvents();

        QString queryName = cleanQueryName(queryNames[i]);
        g_blastSearch->m_blastQueries.addQuery(new BlastQuery(queryName,
                                                              querySequences[i]));
    }
}


QString BlastSearch::cleanQueryName(QString queryName)
{
    //Replace whitespace with underscores
    queryName = queryName.replace(QRegExp("\\s"), "_");

    //Remove any dots from the end of the query name.  BLAST doesn't
    //include them in its results, so if we don't remove them, then
    //we won't be able to find a match between the query name and
    //the BLAST hit.
    while (queryName.length() > 0 && queryName[queryName.size() - 1] == '.')
        queryName = queryName.left(queryName.size() - 1);

    return queryName;
}

void BlastSearch::blastTargetChanged(QString queryName)
{
    g_assemblyGraph->clearAllBlastHitPointers();

    std::vector<BlastQuery *> queries;

    //If "all" is selected, then we'll display each of the BLAST queries
    if (queryName == "all")
        queries = g_blastSearch->m_blastQueries.m_queries;

    //If only one query is selected, then just display that one.
    else
        queries.push_back(g_blastSearch->m_blastQueries.getQueryFromName(queryName));

    //Add the blast hit pointers to nodes that have a hit for
    //the selected target(s).
    for (size_t i = 0; i < queries.size(); ++i)
    {
        BlastQuery * currentQuery = queries[i];
        for (size_t j = 0; j < g_blastSearch->m_hits.size(); ++j)
        {
            BlastHit * hit = &(g_blastSearch->m_hits[j]);
            if (hit->m_query == currentQuery)
                hit->m_node->m_blastHits.push_back(hit);
        }
    }
}