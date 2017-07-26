//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//
// This implements a plain-text deserializer
//

#include "stdafx.h"
#include "Basics.h"
#include "DataDeserializer.h"
#include "ReaderConstants.h"
#include "File.h"

#include <vector>
#include <memory>
#include <unordered_map>

#define let const auto

namespace CNTK {

using namespace std;
using namespace Microsoft::MSR::CNTK;
using namespace msra::strfun;

// helpers
static void LoadTextFileAsCharArray(vector<char>& buf, const wstring& path, uint64_t begin = 0, uint64_t end = SIZE_MAX)
{
    File f(path, fileOptionsRead | fileOptionsBinary);
    if (end == SIZE_MAX)
        end = f.Size();
    f.SetPosition(begin);
    buf.reserve(end - begin + 1); // add 1 byte as courtesy to the caller, which needs it for the 0-terminator
    freadOrDie(buf, end - begin, f);
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// ===========================================================================
// This class implements a plain-text deserializer, for lines of space-separate tokens
// as needed for many language-modeling and translation tasks.
// ===========================================================================

class PlainTextDeserializerImpl : public DataDeserializer
{
public:
    // configuration of each text stream
    const struct PlainTextVocabularyConfiguration
    {
        const std::wstring fileName;
        const std::wstring insertAtStart;
        const std::wstring insertAtEnd;
        const std::wstring substituteForUnknown;
    };
    struct PlainTextStreamConfiguration : public StreamInformation
    {
        PlainTextStreamConfiguration(StreamInformation&& streamInfo, vector<wstring>&& fileNames, PlainTextVocabularyConfiguration&& vocabularyConfig) :
            StreamInformation(move(streamInfo)), m_fileNames(move(fileNames)), m_vocabularyConfig(vocabularyConfig)
        { }
        const vector<wstring> m_fileNames;
        const PlainTextVocabularyConfiguration m_vocabularyConfig;
    };

    // vocabulary (maps grapheme to index)
    class Vocabulary
    {
    public:
        Vocabulary(const PlainTextVocabularyConfiguration& config)
        {
            // load vocab file
            vector<char> buf;
            LoadTextFileAsCharArray(buf, config.fileName);
            // tokenize it
            m_words = split(string(buf.begin(), buf.end()), "\r\n");
            // now put them all into an STL hash map
            m_wordMap.reserve(m_words.size() + 3);
            for (size_t i = 0; i < m_words.size(); i++)
            {
                let res = m_wordMap.insert(make_pair(m_words[i], i));
                if (!res.second)
                    InvalidArgument("Vocabulary: Duplicate word '%s' in vocabulary file %S.", m_words[i].c_str(), config.fileName.c_str());
            }
            // add the special tokens
            // These are supposed to be in the vocabulary, but they are missing in some configs.
            // They are added to the end of the vocab in the order sent-start, sent-end, unknonw-substitute.
            // Note that sent-start and sent-end are only special if they are specified to be added on the fly.
            // Special tokens must only use characters in ASCII range, since we don't know the encoding of the vocab file, which we match as 8-bit strings.
            for (let& specialToken : vector<string>{ AsAsciiString(config.insertAtStart), AsAsciiString(config.insertAtEnd), AsAsciiString(config.substituteForUnknown) })
            {
                if (!specialToken.empty())
                {
                    let res = m_wordMap.insert(make_pair(specialToken, m_words.size()));
                    if (res.second)
                    {
                        fprintf(stderr, "Vocabulary: Special token '%s' missing, adding as index %d. %S.\n", specialToken.c_str(), (int)m_words.size(), config.fileName.c_str());
                        m_words.push_back(specialToken);
                    }
                }
            }
            // precompute the word index of the unknown-substitute token if given
            m_unknownId = config.substituteForUnknown.empty() ? SIZE_MAX : operator()(AsAsciiString(config.substituteForUnknown).c_str());
        }
        size_t operator()(const char* word) const
        {
            let res = m_wordMap.find(word);
            if (res != m_wordMap.end())
                return res->second;
            else if (m_unknownId != SIZE_MAX)
                return m_unknownId;
            else
                InvalidArgument("Vocabulary: Encountered an unknown word when no subsitute was specified:", word);
        }
        size_t size() const { return m_words.size(); }
    private:
        vector<string> m_words;
        unordered_map<string, size_t> m_wordMap;
        size_t m_unknownId;
    };

    // info stored for each text stream
    struct PlainTextStream : public PlainTextStreamConfiguration
    {
        // constructor
        PlainTextStream(const PlainTextStreamConfiguration& streamConfig) :
            PlainTextStreamConfiguration(streamConfig),
            m_vocabulary(streamConfig.m_vocabularyConfig)
        {
            if (NDShape{ m_vocabulary.size() } != m_sampleLayout)
                // BUGBUG: AsString() cannot be called from outside--how to do that?
                InvalidArgument("PlainTextDeserializer: Sample layout %S does not match vocabulary size %d for %S",
                                m_sampleLayout.AsString().c_str(), (int)m_vocabulary.size(), streamConfig.m_vocabularyConfig.fileName);
        }

        // initialization helpers
        // This is not a fast operation, so call this after the quick construction steps have completed and error-checked.
        void IndexAllFiles(int traceLevel)
        {
            // load the vocabulary files
            // --TODO do the indexing later, so that we can first load and check the vocab files before loading big data

            // index all files
            // That is, determine all line offsets and word counts and remember them in m_textLineRefs.
            // TODO: Later this can be cached.
            vector<char> fileAsCString;
            let numWords0 = !m_vocabularyConfig.insertAtStart.empty() + !m_vocabularyConfig.insertAtEnd.empty(); // initial count
            m_textLineRefs.resize(m_fileNames.size());
            m_totalNumWords = 0;
            for (size_t i = 0; i < m_fileNames.size(); i++)
            {
                let& fileName = m_fileNames[i];
                auto& thisFileTextLineRefs = m_textLineRefs[i];
                // load file
                // TODO: This should use smaller memory size; no need to allocate a GB of RAM for this indexing process.
                LoadTextFileAsCharArray(fileAsCString, fileName);
                if (fileAsCString.back() != '\n') // this is required for correctness of the subsequent loop
                {
                    fprintf(stderr, "PlainTextStream: Missing newline character at end of file %S\n", fileName.c_str());
                    fileAsCString.push_back('\n');
                }
                // determine line offsets and per-line word counts
                // This is meant to run over files of GB size and thus be fast.
                size_t numWordsInFile = 0;
                const char* pdata = fileAsCString.data(); // start of buffer
                const char* pend = pdata + fileAsCString.size();
                for (const char* p0 = pdata; p0 < pend; p0++) // loop over lines
                {
                    const char* p; // running pointer
                    for (p = p0; IsSpace(*p); p++) // skip initial spaces
                        ;
                    size_t numWords = numWords0;
                    bool prevIsSpace = true;
                    for (p = p0; *p != '\n'; p++) // count all space/non-space transitions in numWords
                    {
                        let thisIsSpace = IsSpace(*p);
                        numWords += prevIsSpace && !thisIsSpace;
                        prevIsSpace = thisIsSpace;
                    }
                    thisFileTextLineRefs.push_back(TextLineRef{ (uint64_t)(p0 - pdata), numWords });
                    numWordsInFile += numWords;
                    p0 = p; // now points to '\n'; will be increased above
                }
                m_totalNumWords += numWordsInFile;
                // log
                if (traceLevel >= 1)
                    fprintf(stderr, "PlainTextDeserializer: %d words, %d lines. %S\n", (int)numWordsInFile, (int)thisFileTextLineRefs.size(), fileName.c_str());
            }
        }

        // information organized by files
        struct TextLineRef
        {
            const uint64_t beginOffset; // byte offset of this line in the file
            const size_t numWords;      // number of word tokens in the line
        };
        vector<vector<TextLineRef>> m_textLineRefs; // [fileIndex][lineIndex] -> (byte offset of line, #words in this line)
        size_t m_totalNumWords;  // total word count for this stream
        Vocabulary m_vocabulary; // mapping from word strings to neuron indices
    };

    PlainTextDeserializerImpl(const vector<PlainTextDeserializerImpl::PlainTextStreamConfiguration>& streamConfigs, bool cacheIndex,
                              DataType elementType, bool primary, int traceLevel) :
        m_streams(streamConfigs.begin(), streamConfigs.end()), m_cacheIndex(cacheIndex), m_elementType(elementType), m_primary(primary), m_traceLevel(traceLevel)
    {
        if (m_streams.empty())
            InvalidArgument("PlainTextDeserializer: At least one stream must be specified.");

        // index all files
        for (auto& stream : m_streams)
            stream.IndexAllFiles(m_traceLevel);

        // statistics and verify that all files have the same #lines
        m_totalNumLines = 0;
        size_t totalNumSrcWords = 0;
        let& firstFileTextLineRefs = m_streams[0].m_textLineRefs;
        for (size_t j = 0; j < firstFileTextLineRefs.size(); j++)
        {
            m_totalNumLines += firstFileTextLineRefs[j].size();
            totalNumSrcWords += m_streams[0].m_totalNumWords;
        }
        for (size_t i = 1; i < m_streams.size(); i++)
        {
            let& thisFileTextLineRefs  = m_streams[i].m_textLineRefs;
            if (firstFileTextLineRefs.size() != thisFileTextLineRefs.size())
                InvalidArgument("PlainTextDeserializer: All streams must have the same number of files.");
            for (size_t j = 0; j < firstFileTextLineRefs.size(); j++)
                if (firstFileTextLineRefs[j].size() != thisFileTextLineRefs[j].size())
                    InvalidArgument("PlainTextDeserializer: Files across streams must have the same number of lines.");
        }

        if (m_traceLevel >= 1)
            fprintf(stderr, "PlainTextDeserializer: %d files with %d lines and %d words in the first stream out of %d\n",
                (int)m_streams[0].m_textLineRefs.size(), (int)m_totalNumLines, (int)totalNumSrcWords, (int)m_streams.size());
    }

    ///
    /// Gets stream information for all streams this deserializer exposes.
    ///
    virtual std::vector<StreamInformation> StreamInfos() override
    {
        return vector<StreamInformation>(m_streams.begin(), m_streams.end());
    }

    ///
    /// Gets metadata for chunks this deserializer exposes.
    ///
    virtual std::vector<ChunkInfo> ChunkInfos() override
    {
        return std::vector<ChunkInfo>();
    }

    ///
    /// Gets sequence infos for a given a chunk.
    ///
    virtual void SequenceInfosForChunk(ChunkIdType chunkId, std::vector<SequenceInfo>& result) override
    {
        chunkId; result;
    }

    ///
    /// Gets sequence information given the one of the primary deserializer.
    /// Used for non-primary deserializers.
    /// Returns false if the corresponding secondary sequence is not valid.
    ///
    virtual bool GetSequenceInfo(const SequenceInfo& primary, SequenceInfo& result) override
    {
        primary; result;
        return true;
    }

    ///
    /// Gets chunk data given its id.
    ///
    virtual ChunkPtr GetChunk(ChunkIdType chunkId) override
    {
        chunkId;
        return nullptr;
    }

private:
    vector<PlainTextStream> m_streams;
    const bool m_cacheIndex;
    const DataType m_elementType;
    const bool m_primary;
    const int m_traceLevel;

    size_t m_totalNumLines; // total line count for the stream (same for all streams)
};

// factory function to create a PlainTextDeserializer from a V1 config object
shared_ptr<DataDeserializer> CreatePlainTextDeserializer(const ConfigParameters& configParameters, bool primary)
{
    // convert V1 configParameters back to a C++ data structure
    const ConfigParameters& input = configParameters(L"input");
    if (input.empty())
        InvalidArgument("PlainTextDeserializer configuration contains an empty \"input\" section.");

    string precision = configParameters.Find("precision", "float");
    DataType elementType;
    if (precision == "double")
        elementType = DataType::Double;
    else if (precision == "float")
        elementType = DataType::Float;
    else
        RuntimeError("Not supported precision '%s'. Expected 'double' or 'float'.", precision.c_str());

    // enumerate all streams
    vector<PlainTextDeserializerImpl::PlainTextStreamConfiguration> streamConfigs; streamConfigs.reserve(input.size());
    for (const pair<string, ConfigParameters>& section : input)
    {
        ConfigParameters streamConfigParameters = section.second;
        // basic stream info
        StreamInformation streamInfo;
        streamInfo.m_name          = utf16(section.first);
        streamInfo.m_id            = streamConfigs.size(); // id = index of stream config in stream-config array
        streamInfo.m_storageFormat = StorageFormat::SparseCSC;
        streamInfo.m_elementType   = elementType;
        streamInfo.m_sampleLayout  = NDShape{ streamConfigParameters(L"dim") };
        streamInfo.m_definesMbSize = streamConfigParameters(L"definesMBSize", false);
        // list of files. Wildcard expansion is happening here.
        wstring fileNamesArg = streamConfigParameters(L"dataFiles");
#if 1
        // BUGBUG: We pass these pathnames currently as a single string, since passing a vector<wstring> through the V1 Configs gets tripped up by * and :
        fileNamesArg.erase(0, 2); fileNamesArg.pop_back(); fileNamesArg.pop_back(); // strip (\n and )\n
        let fileNames = split(fileNamesArg, L"\n");
#else
        vector<wstring> fileNames = streamConfigParameters(L"dataFiles", ConfigParameters::Array(Microsoft::MSR::CNTK::stringargvector()));
#endif
        vector<wstring> expandedFileNames;
        for (let& fileName : fileNames)
            expand_wildcards(fileName, expandedFileNames);
        // vocabularyConfig
        streamConfigs.emplace_back(PlainTextDeserializerImpl::PlainTextStreamConfiguration
        (
            move(streamInfo),
            move(expandedFileNames),
            {
                streamConfigParameters(L"vocabularyFile"),
                streamConfigParameters(L"insertAtStart", L""),
                streamConfigParameters(L"insertAtEnd", L""),
                streamConfigParameters(L"substituteForUnknown", L"")
            }
        ));
    }

    bool cacheIndex = configParameters(L"cacheIndex");
    let traceLevel = configParameters(L"traceLevel", 1);
    //let chunkSizeBytes = configParameters(L"chunkSizeInBytes", g_32MB); // 32 MB by default   --do we need this?

    // construct the deserializer from that
    return make_shared<PlainTextDeserializerImpl>(move(streamConfigs), cacheIndex, elementType, primary, traceLevel);
}

}; // namespace