//
// Copyright (c) 2017-2026, Manticore Software LTD (https://manticoresearch.com)
// Copyright (c) 2001-2016, Andrew Aksyonoff
// Copyright (c) 2008-2016, Sphinx Technologies Inc
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#include <gtest/gtest.h>

#include "sphinxint.h"
#include "attribute.h"
#include "sphinxrt.h"
#include "sphinxsort.h"
#include "searchdaemon.h"
#include "binlog.h"
#include "accumulator.h"
#include "sphinxudf.h"
#include "sphinxquery/xqparser.h"
#include "indexfiles.h"
#include "sphinxsearch.h"
#include "searchnode.h"
#include "secondaryindex.h"
#include "searchdsql.h"
#include "client_task_info.h"
#include "queryprofile.h"

#include <gmock/gmock.h>


//////////////////////////////////////////////////////////////////////////
static void DeleteIndexFiles ( const char * sIndex )
{
	if ( !sIndex )
		return;

	const char * sExts[] = {
		"kill", "lock", "meta", "ram", "0.spa", "0.spd", "0.spe", "0.sph", "0.spi", "0.spk", "0.spm", "0.spp" };

	CSphString sName;
	for (auto & sExt : sExts)
	{
		sName.SetSprintf ( "%s.%s", sIndex, sExt );
		unlink ( sName.cstr () );
	}
}

static void DeleteAttachTestFiles ( const char * szBase )
{
	CSphString sName;
	for ( const auto & tExt : sphGetExts() )
	{
		sName.SetSprintf ( "%s%s", szBase, tExt.m_szExt );
		unlink ( sName.cstr() );
		sName.SetSprintf ( "%s.0%s", szBase, tExt.m_szExt );
		unlink ( sName.cstr() );
	}

	for ( const char * szExt : { "lock", "meta", "meta.new", "ram", "kill" } )
	{
		sName.SetSprintf ( "%s.%s", szBase, szExt );
		unlink ( sName.cstr() );
	}
}

void TestRTInit ()
{
	CSphConfigSection tRTConfig;

	sphRTInit ( "" );
	Binlog::Configure ( tRTConfig, 0 );

	SmallStringHash_T<CSphIndex *> hIndexes;
	Binlog::Replay ( hIndexes );
}

#define RT_INDEX_FILE_NAME "test_temp"

class MockTestDoc_c : public CSphSource
{
public:
	explicit MockTestDoc_c ( const CSphSchema &tSchema, BYTE ** ppDocs, int iDocs, int iFields )
		: CSphSource ( "test_doc" )
	{
		m_tSchema = tSchema;
		m_ppDocs = ppDocs;
		m_iDocCount = iDocs;
		m_iFields = iFields;
		m_dFieldLengths.Resize ( m_iFields );
		m_dFields.Reserve ( iFields );
	}

	BYTE ** NextDocument ( bool & bEOF, CSphString & ) override
	{
		bEOF = false;
		int iDoc = (int)++m_iDocsCounter;
		if ( iDoc>=m_iDocCount )
		{
			bEOF = true;
			return nullptr;
		}

		for ( int i = 0; i<m_iFields; i++ )
		{
			char * szField = ( char * ) ( m_ppDocs + iDoc * m_iFields )[i];
			m_dFieldLengths[i] = (int) strlen ( szField );
		}

		const CSphColumnInfo * pId = m_tSchema.GetAttr ( sphGetDocidName() );
		assert ( pId );
		m_tDocInfo.SetAttr( pId->m_tLocator, iDoc+1 );

		return m_ppDocs + iDoc * m_iFields;
	}

	MOCK_CONST_METHOD0( GetFieldLengths, const int *() ); // return m_dFieldLengths.Begin();
	MOCK_METHOD1 ( Connect, bool ( CSphString & ) ); // return true;
	MOCK_METHOD0 ( Disconnect, void() );

	bool IterateStart ( CSphString & ) final
	{
		m_tDocInfo.Reset ( m_tSchema.GetRowSize () );
		m_iPlainFieldsLength = m_tSchema.GetFieldsCount();
		m_iDocsCounter = -1;
		return true;
	}

	MOCK_METHOD2 ( IterateMultivaluedStart, bool ( int, CSphString& )); // return false;
	MOCK_METHOD2 ( IterateMultivaluedNext, bool(int64_t &, int64_t &)); // return false;
	MOCK_METHOD1 ( IterateKillListStart, bool (CSphString & ) ); // return false;
	MOCK_METHOD1 ( IterateKillListNext, bool (DocID_t & ) ) ; // return false
	int GetFieldCount () const { return m_iFields; }

	CSphVector<VecTraits_T<const char>> GetFields ()
	{
		m_dFields.Resize(0);
		for ( int i=0; i<m_iFields; ++i)
		{
			auto pStr = (const char*) m_ppDocs[m_iDocsCounter*m_iFields + i];
			m_dFields.Add ( VecTraits_T<const char> (pStr,strlen(pStr)));
		}
		return m_dFields;
	}

	int m_iDocsCounter;
	int m_iDocCount;
	int m_iFields;
	BYTE ** m_ppDocs;
	CSphVector<VecTraits_T<const char> > m_dFields;
	CSphVector<int> m_dFieldLengths;
};


class MockDocRandomizer_c : public CSphSource
{
public:
	static const int m_iMaxFields = 2;
	static const int m_iMaxFieldLen = 512;
	char m_dFields[m_iMaxFields][m_iMaxFieldLen];
	char * m_ppFields[m_iMaxFields];
	CSphVector<VecTraits_T<const char> > m_dMeasuredFields;
	int m_dFieldLengths[m_iMaxFields];
	int	m_iDocsCounter;

	explicit MockDocRandomizer_c ( const CSphSchema & tSchema ) : CSphSource ( "test_doc" )
	{
		m_tSchema = tSchema;
		m_dMeasuredFields.Reserve(m_iMaxFields);
		for ( int i=0; i<m_iMaxFields; ++i )
			m_ppFields[i] = (char *)&m_dFields[i];
	}

	BYTE ** NextDocument ( bool & bEOF, CSphString & ) override
	{
		bEOF = false;
		if ( m_iDocsCounter>800 )
		{
			bEOF = true;
			return nullptr;
		}

		++m_tDocInfo.m_tRowID;
		++m_iDocsCounter;

		m_tDocInfo.SetAttr ( m_tSchema.GetAttr(0).m_tLocator, m_tDocInfo.m_tRowID+1000 );
		m_tDocInfo.SetAttr ( m_tSchema.GetAttr(1).m_tLocator, 1313 );

		snprintf ( m_dFields[0], m_iMaxFieldLen, "cat title%d title%d title%d title%d title%d"
			, sphRand(), sphRand(), sphRand(), sphRand(), sphRand() );

		snprintf ( m_dFields[1], m_iMaxFieldLen, "dog contentwashere%d contentwashere%d contentwashere%d contentwashere%d contentwashere%d"
			, sphRand(), sphRand(), sphRand(), sphRand(), sphRand() );

		for ( int i=0; i < m_iMaxFields; ++i )
			m_dFieldLengths[i] = (int) strlen ( m_ppFields[i] );

		return (BYTE**) &m_ppFields[0];
	}


	MOCK_CONST_METHOD0( GetFieldLengths, const int *() ); // return m_dFieldLengths.Begin();
	MOCK_METHOD1 ( Connect, bool ( CSphString & ) ); // return true;
	MOCK_METHOD0 ( Disconnect, void () );

	bool IterateStart ( CSphString & ) final
	{
		m_tDocInfo.Reset ( m_tSchema.GetRowSize () );
		m_iDocsCounter = 0;
		m_iPlainFieldsLength = m_tSchema.GetFieldsCount();
		return true;
	}

	MOCK_METHOD2 ( IterateMultivaluedStart, bool ( int, CSphString & ) ); // return false;
	MOCK_METHOD2 ( IterateMultivaluedNext, bool (int64_t &, int64_t &) ); // return false;
	MOCK_METHOD1 ( IterateKillListStart, bool (CSphString & ) ); // return false;
	MOCK_METHOD1 ( IterateKillListNext, bool (DocID_t & ) ); // return false
	int  GetFieldCount () const { return m_iMaxFields; }

	CSphVector<VecTraits_T<const char>> GetFields ()
	{
		m_dMeasuredFields.Resize ( 0 );
		for ( const char * pStr : m_ppFields )
			m_dMeasuredFields.Add ( VecTraits_T<const char> ( pStr, strlen ( pStr ) ) );
		return m_dMeasuredFields;
	}
};


//////////////////////////////////////////////////////////////////////////



class RT : public ::testing::Test
{

protected:
	void SetUp() override
	{
		DeleteIndexFiles ( RT_INDEX_FILE_NAME );
		TestRTInit();
		tDictSettings.SetDictFormat ( DictFormat_e::CRC );

		pTok = Tokenizer::Detail::CreateUTF8Tokenizer ();

		tSrcSchema.Reset ();
		tSrcSchema.AddField ( "title" );
		tSrcSchema.AddField ( "content" );
	}

	void TearDown() override
	{
		Binlog::Deinit ();
		DeleteIndexFiles ( RT_INDEX_FILE_NAME );
	}

	CSphColumnInfo tCol;
	CSphSchema tSrcSchema;
	CSphString sError, sWarning;

	TokenizerRefPtr_c pTok;

	CSphDictSettings tDictSettings;
};

TEST_F ( RT, AttachReportsMetadataFailure )
{
	static constexpr const char * SOURCE_PATH = "test_attach_source";
	static constexpr const char * TARGET_PATH = "test_attach_target";
	DeleteAttachTestFiles ( SOURCE_PATH );
	DeleteAttachTestFiles ( TARGET_PATH );

	Threads::CallCoroutine ( [&] {
		tCol.m_sName = "id";
		tCol.m_eAttrType = SPH_ATTR_BIGINT;
		tSrcSchema.AddAttr ( tCol, true );

		CSphSchema tSchema;
		for ( int i=0; i<tSrcSchema.GetFieldsCount(); ++i )
			tSchema.AddField ( tSrcSchema.GetField(i) );
		for ( int i=0; i<tSrcSchema.GetAttrsCount(); ++i )
			tSchema.AddAttr ( tSrcSchema.GetAttr(i), false );

		auto pDict = sphCreateDictionaryCRC ( tDictSettings, nullptr, pTok, "attach", false, 32, nullptr, sError );
		ASSERT_TRUE ( pDict );

		auto fnCreateRt = [&] ( const char * szName, const char * szPath ) {
			auto pIndex = sphCreateIndexRT ( szName, szPath, tSchema, 128 * 1024 );
			pIndex->SetTokenizer ( pTok->Clone ( SPH_CLONE_INDEX ) );
			pIndex->SetDictionary ( pDict->Clone() );
			pIndex->PostSetup();
			return pIndex;
		};

		auto pSource = fnCreateRt ( "attach_source", SOURCE_PATH );
		StrVec_t dWarnings;
		ASSERT_TRUE ( pSource->Prealloc ( false, nullptr, dWarnings ) );

		InsertDocData_c tDoc ( pSource->GetMatchSchema() );
		tDoc.SetID ( 101 );
		const char * szTitle = "attached row";
		const char * szContent = "metadata failure";
		tDoc.m_dFields.Add ( { szTitle, (int64_t)strlen ( szTitle ) } );
		tDoc.m_dFields.Add ( { szContent, (int64_t)strlen ( szContent ) } );

		RtAccum_t tAcc;
		CSphString sFilter;
		ASSERT_TRUE ( pSource->AddDocument ( tDoc, false, sFilter, sError, sWarning, &tAcc ) );
		ASSERT_TRUE ( pSource->Commit ( nullptr, &tAcc, &sError ) );
		ASSERT_TRUE ( pSource->ForceDiskChunk() );
		pSource.reset();

		auto pPlain = sphCreateIndexPhrase ( "attach_plain", SphSprintf ( "%s.0", SOURCE_PATH ) );
		dWarnings.Reset();
		ASSERT_TRUE ( pPlain->Prealloc ( false, nullptr, dWarnings ) );

		auto pTarget = fnCreateRt ( "attach_target", TARGET_PATH );
		dWarnings.Reset();
		ASSERT_TRUE ( pTarget->Prealloc ( false, nullptr, dWarnings ) );
		pTarget->ProhibitSave();

		bool bFatal = false;
		sError = "";
		auto eResult = pTarget->AttachDiskIndexWithMeta ( pPlain.get(), false, bFatal, sError );
		if ( eResult!=DiskAttachRes_e::NOT_ATTACHED )
			pPlain.release();

		EXPECT_EQ ( eResult, DiskAttachRes_e::META_FAILED );
		EXPECT_FALSE ( bFatal );
		EXPECT_THAT ( sError.cstr(), testing::HasSubstr ( "failed to save metadata" ) );
		EXPECT_EQ ( pTarget->GetStats().m_iTotalDocuments, 1 );
		pTarget->EnableSave();
	} );

	DeleteAttachTestFiles ( SOURCE_PATH );
	DeleteAttachTestFiles ( TARGET_PATH );
}

/*
 * It was instantiated several times, but that wasn't work, since on every instantiation couple of attributes was inserted into schema, having idex's schema the same.
 */

TEST_F ( RT, WeightBoundary )
{
	DWORD uParam = 1500;
	using namespace testing;
	Threads::CallCoroutine ( [&] {
	DictRefPtr_c pDict { sphCreateDictionaryCRC ( tDictSettings, nullptr, pTok, "weight", false, 32, nullptr, sError ) };

	tCol.m_sName = "id";
	tCol.m_eAttrType = SPH_ATTR_BIGINT;
	tSrcSchema.AddAttr ( tCol, true );

	tCol.m_sName = "channel_id";
	tCol.m_eAttrType = SPH_ATTR_INTEGER;
	tSrcSchema.AddAttr ( tCol, true );

	const char * dFields[] = { "If I were a cat...", "We are the greatest cat" };
	auto * pSrc = new MockTestDoc_c ( tSrcSchema, ( BYTE ** ) dFields, 1, 2 );

	EXPECT_CALL ( *pSrc, Connect ( _ ) ).WillOnce ( Return ( true ) );
	EXPECT_CALL ( *pSrc, GetFieldLengths () ).WillOnce ( Return ( pSrc->m_dFieldLengths.Begin () ) );
	EXPECT_CALL ( *pSrc, Disconnect () );

	pSrc->SetTokenizer ( pTok );
	pSrc->SetDict ( pDict );
	pSrc->Setup ( CSphSourceSettings(), nullptr );

	EXPECT_TRUE ( pSrc->Connect ( sError ) );
	EXPECT_TRUE ( pSrc->IterateStart ( sError ) );
	EXPECT_TRUE ( pSrc->UpdateSchema ( &tSrcSchema, sError ) );

	CSphSchema tSchema; // source schema must be all dynamic attrs; but index ones must be static
	for ( int i=0; i<tSrcSchema.GetFieldsCount(); i++ )
		tSchema.AddField ( tSrcSchema.GetField(i) );

	for ( int i=0; i<tSrcSchema.GetAttrsCount(); i++ )
		tSchema.AddAttr ( tSrcSchema.GetAttr(i), false );

	auto pIndex = sphCreateIndexRT ( "testrt", RT_INDEX_FILE_NAME, tSchema, 32 * 1024 * 1024 );

	// tricky bit
	// index owns its tokenizer/dict pair, and MAY do whatever it wants
	// and starting with meta v4, it WILL deallocate tokenizer/dict in Prealloc()
	// in favor of tokenizer/dict loaded from the saved settings in meta
	// however, source still needs those guys!
	// so for simplicity i just clone them
	pIndex->SetTokenizer ( pTok->Clone ( SPH_CLONE_INDEX ) );
	pIndex->SetDictionary ( pDict->Clone () );
	pIndex->PostSetup ();
	StrVec_t dWarnings;
	EXPECT_TRUE ( pIndex->Prealloc ( false, nullptr, dWarnings ) );

	InsertDocData_c tDoc ( pIndex->GetMatchSchema() );
	int iDynamic = pIndex->GetMatchSchema().GetRowSize();

	RtAccum_t tAcc;

	CSphString sFilter;
	bool bEOF = false;
	while (true)
	{
		EXPECT_TRUE ( pSrc->IterateDocument ( bEOF, sError ) );
		if ( bEOF )
			break;

		tDoc.m_dFields = pSrc->GetFields();
		tDoc.m_tDoc.Combine ( pSrc->m_tDocInfo, iDynamic );
		pIndex->AddDocument ( tDoc, false, sFilter, sError, sWarning, &tAcc );
		pIndex->Commit ( nullptr, &tAcc );
	}

	pSrc->Disconnect ();

	ASSERT_EQ ( pSrc->GetStats ().m_iTotalDocuments, 1) << "docs committed";

	CSphQuery tQuery;
	AggrResult_t tResult;
	CSphQueryResult tQueryResult;
	tQueryResult.m_pMeta = &tResult;
	CSphMultiQueryArgs tArgs ( 1 );
	tQuery.m_sQuery = "@title cat";
	auto pParser = sphCreatePlainQueryParser();
	tQuery.m_pQueryParser = pParser.get();

	SphQueueSettings_t tQueueSettings ( pIndex->GetMatchSchema () );
	SphQueueRes_t tRes;
	ISphMatchSorter * pSorter = sphCreateQueue ( tQueueSettings, tQuery, tResult.m_sError, tRes );
	ASSERT_TRUE ( pSorter );
	ASSERT_TRUE ( pIndex->MultiQuery ( tQueryResult, tQuery, { &pSorter, 1 }, tArgs ) );
	auto & tOneRes = tResult.m_dResults.Add ();
	tOneRes.FillFromSorter ( pSorter );
	ASSERT_EQ ( tResult.GetLength (), 1 ) << "results found";
	ASSERT_EQ ( tOneRes.m_dMatches[0].m_tRowID, 0 ) << "rowID" ;
	ASSERT_EQ ( tOneRes.m_dMatches[0].m_iWeight, uParam) << "weight" ;

	SafeDelete ( pSorter );
	SafeDelete ( pSrc );
	});
}


TEST_F ( RT, RankerFactors )
{
	using namespace testing;
	Threads::CallCoroutine ( [&] {

	const char * dFields[] = {
		"Seven lies multiplied by seven", "", "Multiplied by seven again", "", "Seven lies multiplied by seven"
		, "Multiplied by seven again", "Mary vs Lamb", "Mary had a little lamb little lamb little lamb"
		, "Mary vs Lamb 2: Return of The Lamb", "...whose fleece was white as snow", "Mary vs Lamb 3: The Resurrection"
		, "Snow! Bloody snow!", "the who", "what the foo"
	};
	const char * dQueries[] = {
		"seven !(angels !by)", // matched by 0-2
		"Mary lamb", // matched by 3-5
		"(the who) | (the foo)", // matched by 6
	};

	tCol.m_sName = "id";
	tCol.m_eAttrType = SPH_ATTR_BIGINT;
	tSrcSchema.AddAttr ( tCol, true );

	tCol.m_sName = "idd";
	tCol.m_eAttrType = SPH_ATTR_INTEGER;
	tSrcSchema.AddAttr ( tCol, true );

	auto pDict = sphCreateDictionaryCRC ( tDictSettings, NULL, pTok, "rt", false, 32, nullptr, sError );

	auto pSrc = new MockTestDoc_c ( tSrcSchema, ( BYTE ** ) dFields, sizeof ( dFields ) / sizeof ( dFields[0] ) / 2
									, 2 );

	EXPECT_CALL ( *pSrc, Connect ( _ ) ).WillOnce ( Return ( true ) );
	EXPECT_CALL ( *pSrc, GetFieldLengths () ).Times ( 7 ).WillRepeatedly ( Return ( pSrc->m_dFieldLengths.Begin () ) );
	EXPECT_CALL ( *pSrc, Disconnect () );

	pSrc->SetTokenizer ( pTok );
	pSrc->SetDict ( pDict );

	pSrc->Setup ( CSphSourceSettings(), nullptr );
	ASSERT_TRUE ( pSrc->Connect ( sError ) );
	ASSERT_TRUE ( pSrc->IterateStart ( sError ) );

	ASSERT_TRUE ( pSrc->UpdateSchema ( &tSrcSchema, sError ) );

	CSphSchema tSchema; // source schema must be all dynamic attrs; but index ones must be static
	for ( int i=0; i<tSrcSchema.GetFieldsCount(); i++ )
		tSchema.AddField ( tSrcSchema.GetField(i) );

	for ( int i=0; i<tSrcSchema.GetAttrsCount(); i++ )
		tSchema.AddAttr ( tSrcSchema.GetAttr(i), false );

	auto pIndex = sphCreateIndexRT ( "testrt", RT_INDEX_FILE_NAME, tSchema, 128 * 1024 );

	pIndex->SetTokenizer ( pTok ); // index will own this pair from now on
	pIndex->SetDictionary ( sphCreateDictionaryCRC ( tDictSettings, nullptr, pTok, "rt", false, 32, nullptr, sError ) );
	pIndex->PostSetup ();
	StrVec_t dWarnings;
	Verify ( pIndex->Prealloc ( false, nullptr, dWarnings ) );

	CSphString sFilter;
	InsertDocData_c tDoc ( pIndex->GetMatchSchema() );
	int iDynamic = pIndex->GetMatchSchema().GetRowSize();

	RtAccum_t tAcc;
	bool bEOF = false;
	while (true)
	{
		Verify ( pSrc->IterateDocument ( bEOF, sError ) );
		if ( bEOF )
			break;

		tDoc.m_dFields = pSrc->GetFields();
		tDoc.m_tDoc.Combine ( pSrc->m_tDocInfo, iDynamic );
		pIndex->AddDocument ( tDoc, false, sFilter, sError, sWarning, &tAcc );
	}
	pIndex->Commit ( nullptr, &tAcc );
	pSrc->Disconnect ();

	CSphQuery tQuery;
	CSphQueryItem &tFactor = tQuery.m_dItems.Add ();
	tFactor.m_sExpr = "packedfactors()";
	tFactor.m_sAlias = "pf";
	tQuery.m_sRankerExpr = "1";
	tQuery.m_eRanker = SPH_RANK_EXPR;
	tQuery.m_eMode = SPH_MATCH_EXTENDED2;
	tQuery.m_eSort = SPH_SORT_EXTENDED;
	tQuery.m_sSortBy = "@weight desc";
	tQuery.m_sOrderBy = "@weight desc";
	auto pParser = sphCreatePlainQueryParser();
	tQuery.m_pQueryParser = pParser.get();
	AggrResult_t tResult;
	CSphQueryResult tQueryResult;
	tQueryResult.m_pMeta = &tResult;
	CSphMultiQueryArgs tArgs ( 1 );
	tArgs.m_uPackedFactorFlags = SPH_FACTOR_ENABLE | SPH_FACTOR_CALC_ATC;

	SphQueueSettings_t tQueueSettings ( pIndex->GetMatchSchema () );
	tQueueSettings.m_bComputeItems = true;
	SphQueueRes_t tRes;

	for ( auto szQuery : dQueries )
	{
		tQuery.m_sQuery = szQuery;

		auto pSorter = sphCreateQueue ( tQueueSettings, tQuery, tResult.m_sError, tRes );
		ASSERT_TRUE ( pSorter );
		ASSERT_TRUE ( pIndex->MultiQuery ( tQueryResult, tQuery, { &pSorter, 1 }, tArgs ) );
		auto & tOneRes = tResult.m_dResults.Add ();
		tOneRes.FillFromSorter ( pSorter );

		tResult.m_tSchema = *pSorter->GetSchema ();
		const CSphAttrLocator &tLoc = tResult.m_tSchema.GetAttr ( "pf" )->m_tLocator;

		for ( int iMatch = 0; iMatch<tOneRes.m_dMatches.GetLength (); ++iMatch )
		{
			const BYTE * pAttr = (const BYTE *) tOneRes.m_dMatches[iMatch].GetAttr ( tLoc );
			ASSERT_TRUE ( pAttr );

			auto * pFactors = (const unsigned int *) sphUnpackPtrAttr ( pAttr ).first;

			SPH_UDF_FACTORS tUnpacked;
			sphinx_factors_init ( &tUnpacked );
			sphinx_factors_unpack ( pFactors, &tUnpacked );

			// doc level factors
			ASSERT_EQ ( tUnpacked.doc_bm25, sphinx_get_doc_factor_int ( pFactors, SPH_DOCF_BM25 ) );
			ASSERT_EQ ( tUnpacked.doc_bm25a, sphinx_get_doc_factor_float ( pFactors, SPH_DOCF_BM25A ) );
			ASSERT_EQ ( tUnpacked.matched_fields, sphinx_get_doc_factor_int ( pFactors, SPH_DOCF_MATCHED_FIELDS ) );
			ASSERT_EQ ( tUnpacked.doc_word_count, sphinx_get_doc_factor_int ( pFactors, SPH_DOCF_DOC_WORD_COUNT ) );
			ASSERT_EQ ( tUnpacked.num_fields, sphinx_get_doc_factor_int ( pFactors, SPH_DOCF_NUM_FIELDS ) );
			ASSERT_EQ ( tUnpacked.max_uniq_qpos, sphinx_get_doc_factor_int ( pFactors, SPH_DOCF_MAX_UNIQ_QPOS ) );

			// field level factors
			for ( int iField = 0; iField<tUnpacked.num_fields; ++iField )
			{
				if ( !tUnpacked.field[iField].hit_count )
					continue;

				const unsigned int * pField = sphinx_get_field_factors ( pFactors, iField );
				ASSERT_TRUE ( pField );
				ASSERT_EQ ( tUnpacked.field[iField].hit_count, sphinx_get_field_factor_int ( pField
																							 , SPH_FIELDF_HIT_COUNT ) );
				ASSERT_EQ ( tUnpacked.field[iField].lcs, sphinx_get_field_factor_int ( pField, SPH_FIELDF_LCS ) );
				ASSERT_EQ ( tUnpacked.field[iField].word_count, sphinx_get_field_factor_int ( pField
																							  , SPH_FIELDF_WORD_COUNT ) );
				ASSERT_EQ ( tUnpacked.field[iField].tf_idf, sphinx_get_field_factor_float ( pField
																							, SPH_FIELDF_TF_IDF ) );
				ASSERT_EQ ( tUnpacked.field[iField].min_idf, sphinx_get_field_factor_float ( pField
																							 , SPH_FIELDF_MIN_IDF ) );
				ASSERT_EQ (
					tUnpacked.field[iField].max_idf, sphinx_get_field_factor_float ( pField, SPH_FIELDF_MAX_IDF ) );
				ASSERT_EQ (
					tUnpacked.field[iField].sum_idf, sphinx_get_field_factor_float ( pField, SPH_FIELDF_SUM_IDF ) );
				ASSERT_EQ ( tUnpacked.field[iField].min_hit_pos, sphinx_get_field_factor_int ( pField
																							   , SPH_FIELDF_MIN_HIT_POS ) );
				ASSERT_EQ ( tUnpacked.field[iField].min_best_span_pos, sphinx_get_field_factor_int ( pField
																									 , SPH_FIELDF_MIN_BEST_SPAN_POS ) );
				ASSERT_EQ ( tUnpacked.field[iField].max_window_hits, sphinx_get_field_factor_int ( pField
																								   , SPH_FIELDF_MAX_WINDOW_HITS ) );
				ASSERT_EQ (
					tUnpacked.field[iField].min_gaps, sphinx_get_field_factor_int ( pField, SPH_FIELDF_MIN_GAPS ) );
				ASSERT_EQ ( tUnpacked.field[iField].atc, sphinx_get_field_factor_float ( pField, SPH_FIELDF_ATC ) );
				ASSERT_EQ ( tUnpacked.field[iField].lccs, sphinx_get_field_factor_int ( pField, SPH_FIELDF_LCCS ) );
				ASSERT_EQ ( tUnpacked.field[iField].wlccs, sphinx_get_field_factor_float ( pField, SPH_FIELDF_WLCCS ) );
				bool bExactHitSame = ( ( ( tUnpacked.field[iField].exact_hit << iField )
					& sphinx_get_doc_factor_int ( pFactors, SPH_DOCF_EXACT_HIT_MASK ) )!=0 );
				ASSERT_TRUE ( tUnpacked.field[iField].exact_hit==0 || bExactHitSame );
				bool bExactOrderSame = ( ( ( tUnpacked.field[iField].exact_order << iField )
					& sphinx_get_doc_factor_int ( pFactors, SPH_DOCF_EXACT_ORDER_MASK ) )!=0 );
				ASSERT_TRUE ( tUnpacked.field[iField].exact_order==0 || bExactOrderSame );
			}

			// term level factors
			for ( int iWord = 0; iWord<tUnpacked.max_uniq_qpos; iWord++ )
			{
				if ( !tUnpacked.term[iWord].keyword_mask )
					continue;

				const unsigned int * pTerm = sphinx_get_term_factors ( pFactors, iWord + 1 );
				ASSERT_TRUE ( pTerm );
				ASSERT_EQ ( tUnpacked.term[iWord].tf, sphinx_get_term_factor_int ( pTerm, SPH_TERMF_TF ) );
				ASSERT_EQ ( tUnpacked.term[iWord].idf, sphinx_get_term_factor_float ( pTerm, SPH_TERMF_IDF ) );
			}

			sphinx_factors_deinit ( &tUnpacked );

			SafeDelete ( pSorter );
		}
	}

	SafeDelete ( pSrc );
	pTok = nullptr; // owned and deleted by index
	});
}


TEST_F ( RT, SendVsMerge )
{
	using namespace testing;
	Threads::CallCoroutine ( [&] {

	auto pDict = sphCreateDictionaryCRC ( tDictSettings, NULL, pTok, "rt", false, 32, nullptr, sError );

	tCol.m_sName = "id";
	tCol.m_eAttrType = SPH_ATTR_BIGINT;
	tSrcSchema.AddAttr ( tCol, true );

	tCol.m_sName = "tag1";
	tCol.m_eAttrType = SPH_ATTR_INTEGER;
	tSrcSchema.AddAttr ( tCol, true );

	tCol.m_sName = "tag2";
	tCol.m_eAttrType = SPH_ATTR_INTEGER;
	tSrcSchema.AddAttr ( tCol, true );

	auto pSrc = new MockDocRandomizer_c ( tSrcSchema );

	EXPECT_CALL ( *pSrc, Connect ( _ ) ).WillOnce ( Return ( true ) );
	EXPECT_CALL ( *pSrc, GetFieldLengths () ).Times ( 801 ).WillRepeatedly ( Return ( pSrc->m_dFieldLengths ) );
	EXPECT_CALL ( *pSrc, Disconnect () );

	pSrc->SetTokenizer ( pTok );
	pSrc->SetDict ( pDict );

	pSrc->Setup ( CSphSourceSettings(), nullptr );
	ASSERT_TRUE ( pSrc->Connect ( sError ) );
	ASSERT_TRUE ( pSrc->IterateStart ( sError ) );

	ASSERT_TRUE ( pSrc->UpdateSchema ( &tSrcSchema, sError ) );

	CSphSchema tSchema; // source schema must be all dynamic attrs; but index ones must be static
	for ( int i=0; i<tSrcSchema.GetFieldsCount(); i++ )
		tSchema.AddField ( tSrcSchema.GetField(i) );

	for ( int i=0; i<tSrcSchema.GetAttrsCount(); i++ )
		tSchema.AddAttr ( tSrcSchema.GetAttr(i), false );

	auto pIndex = sphCreateIndexRT ( "testrt", RT_INDEX_FILE_NAME, tSchema, 128 * 1024 );

	pIndex->SetTokenizer ( pTok ); // index will own this pair from now on
	pIndex->SetDictionary ( pDict );
	pIndex->PostSetup ();
	StrVec_t dWarnings;
	ASSERT_TRUE ( pIndex->Prealloc ( false, nullptr, dWarnings ) );

	CSphQuery tQuery;
	AggrResult_t tResult;
	CSphQueryResult tQueryResult;
	tQueryResult.m_pMeta = &tResult;
	CSphMultiQueryArgs tArgs ( 1 );
	tQuery.m_sQuery = "@title cat";
	auto pParser = sphCreatePlainQueryParser();
	tQuery.m_pQueryParser = pParser.get();

	CSphQueryItem & tItem = tQuery.m_dItems.Add ();
	tItem.m_sExpr = "*";
	tItem.m_sAlias = "*";
	tQuery.m_sSelect = "*";

	SphQueueSettings_t tQueueSettings ( pIndex->GetMatchSchema () );
	tQueueSettings.m_bComputeItems = true;
	SphQueueRes_t tRes;
	auto pSorter = sphCreateQueue ( tQueueSettings, tQuery, tResult.m_sError, tRes );
	ASSERT_TRUE ( pSorter );

	CSphString sFilter;
	InsertDocData_c tDoc ( pIndex->GetMatchSchema() );
	int iDynamic = pIndex->GetMatchSchema().GetRowSize();

	RtAccum_t tAcc;

	bool bEOF = false;
	while (true)
	{
		ASSERT_TRUE ( pSrc->IterateDocument ( bEOF, sError ) );
		if ( bEOF )
			break;

		tDoc.m_dFields = pSrc->GetFields();
		tDoc.m_tDoc.Combine ( pSrc->m_tDocInfo, iDynamic );
		pIndex->AddDocument ( tDoc, false, sFilter, sError, sWarning, &tAcc );
		sError = ""; // need to reset error message
		if ( pSrc->m_iDocsCounter==350 )
		{
			pIndex->Commit ( NULL, &tAcc );
			EXPECT_TRUE ( pIndex->MultiQuery ( tQueryResult, tQuery, { &pSorter, 1 }, tArgs ) );
			auto & tOneRes = tResult.m_dResults.Add ();
			tOneRes.FillFromSorter ( pSorter );
		}
	}
	pIndex->Commit ( NULL, &tAcc );

	pSrc->Disconnect ();

	tResult.m_tSchema = *pSorter->GetSchema ();

	auto & tOneRes = tResult.m_dResults.First ();
	ASSERT_EQ ( tResult.GetLength (), 20 );
	for ( int i = 0; i<tResult.GetLength (); ++i )
	{
		const RowID_t uID = tOneRes.m_dMatches[i].m_tRowID;
		const SphAttr_t tTag1 = tOneRes.m_dMatches[i].GetAttr ( tResult.m_tSchema.GetAttr ( 0 ).m_tLocator );
		const SphAttr_t tTag2 = tOneRes.m_dMatches[i].GetAttr ( tResult.m_tSchema.GetAttr ( 1 ).m_tLocator );
		ASSERT_TRUE ( ( RowID_t ) tTag1==uID + 1000 );
		ASSERT_TRUE ( tTag2==1313 );
	}

	SafeDelete ( pSorter );
	SafeDelete ( pSrc );
	pTok = nullptr; // owned and deleted by index
	});
}

namespace
{
// Ordered fake postings expose exactly which rows reach positional evaluation.
class CandidateTestWord_c final : public ISphQword
{
public:
	explicit CandidateTestWord_c ( std::initializer_list<RowID_t> dRows );
	const CSphMatch & GetNextDoc() override;
	void SeekHitlist ( SphOffset_t uOff ) override;
	Hitpos_t GetNextHit() override;
	CSphVector<RowID_t> m_dSought;

private:
	CSphVector<RowID_t> m_dRows;
	int m_iNext = 0;
	bool m_bHit = false;
	CSphMatch m_tDoc;
};

/// Copy ordered dRows into a fake word; constructs a reader with one hit per row.
CandidateTestWord_c::CandidateTestWord_c ( std::initializer_list<RowID_t> dRows )
{
	for ( auto tRowID : dRows )
		m_dRows.Add(tRowID);
	m_iDocs = m_dRows.GetLength();
	m_sWord = "test";
}

/// No arguments; returns the next borrowed match, or one with INVALID_ROWID at EOF.
const CSphMatch & CandidateTestWord_c::GetNextDoc()
{
	m_tDoc.m_tRowID = m_iNext<m_dRows.GetLength() ? m_dRows[m_iNext++] : INVALID_ROWID;
	m_dQwordFields.Set(0);
	m_uMatchHits = 1;
	m_iHitlistPos = m_tDoc.m_tRowID;
	return m_tDoc;
}

/// Record uOff (the fake row ID) and arm its single positional hit; returns nothing.
void CandidateTestWord_c::SeekHitlist ( SphOffset_t uOff )
{
	m_dSought.Add(RowID_t(uOff));
	m_bHit = true;
}

/// No arguments; returns the armed position once, then EMPTY_HIT.
Hitpos_t CandidateTestWord_c::GetNextHit()
{
	return std::exchange ( m_bHit, false ) ? 1 : EMPTY_HIT;
}

class CandidateTestSetup_c final : public ISphQwordSetup
{
	/// Ignore the keyword; return nullptr because the test supplies its own word.
	ISphQword * QwordSpawn ( const XQKeyword_t & ) const override { return nullptr; }
	/// Ignore the supplied word; return true because the fake is already initialized.
	bool QwordSetup ( ISphQword * ) const override { return true; }
	/// Ignore the atom position; return nullptr because tests do not spawn scan words.
	ISphQword * ScanSpawn ( int ) const override { return nullptr; }
};

class CandidateTestIterator_c final : public RowidIterator_i
{
public:
	/// Construct an iterator over row IDs [0,iCount), with no cutoff or descriptions.
	explicit CandidateTestIterator_c ( int iCount ) : m_iCount(iCount) {}
	bool HintRowID ( RowID_t tRowID ) override;
	bool GetNextRowIdBlock ( RowIdBlock_t & dBlock ) override;
	/// No arguments; return the next source row ID as the processed-row count.
	int64_t GetNumProcessed() const override { return m_iNext; }
	/// Ignore the cutoff argument for an unbounded fake; return nothing.
	void SetCutoff ( int ) override {}
	/// No arguments; return false because this fake never applies a cutoff.
	bool WasCutoffHit() const override { return false; }
	/// Leave the supplied description vector untouched; return nothing.
	void AddDesc ( CSphVector<IteratorDesc_t> & ) const override {}

private:
	int m_iCount;
	int m_iNext = 0;
	RowID_t m_dBlock[997]; // deliberately does not divide the candidate bound
};

/// Skip rows below tRowID; return whether at least one fake row remains.
bool CandidateTestIterator_c::HintRowID ( RowID_t tRowID )
{
	m_iNext = Max ( m_iNext, int(tRowID) );
	return m_iNext<m_iCount;
}

/// Set dBlock to a borrowed batch of <=997 rows; return false on exhaustion.
bool CandidateTestIterator_c::GetNextRowIdBlock ( RowIdBlock_t & dBlock )
{
	int iCount = Min ( 997, m_iCount-m_iNext );
	for ( int i=0; i<iCount; ++i )
		m_dBlock[i] = m_iNext++;
	dBlock = RowIdBlock_t ( m_dBlock, iCount );
	return iCount>0;
}
}

// Repeated calls must reuse a RAM posting that overshot a missing candidate.
TEST ( MatchCandidates, ReuseOvershootingRamPosting )
{
	Threads::CallCoroutine ( [] {
		MatchCandidates_t tCandidates;
		for ( RowID_t tRowID : { 0U, 1U, 4U, 5U, 8U, 12U } )
			tCandidates.m_dRows.Add(tRowID);
		tCandidates.m_bActive = true;
		CandidateTestWord_c tWord ( { 0, 5, 8, 10 } );
		MatchCandidateCursor_t tCursor;
		tCursor.Reset(&tCandidates);
		EXPECT_EQ ( tCursor.Next(tWord), 0U );
		EXPECT_EQ ( tCursor.Next(tWord), 5U );
		EXPECT_EQ ( tCursor.Next(tWord), 8U );
		EXPECT_EQ ( tCursor.Next(tWord), INVALID_ROWID );
		EXPECT_EQ ( tCursor.Next(tWord), INVALID_ROWID );
	} );
}

// Hints on one leaf must not consume another leaf's candidate position.
TEST ( MatchCandidates, IndependentCursorsAndHints )
{
	Threads::CallCoroutine ( [] {
		MatchCandidates_t tCandidates;
		for ( RowID_t tRowID : { 1U, 3U, 8U } )
			tCandidates.m_dRows.Add(tRowID);
		tCandidates.m_bActive = true;
		CandidateTestWord_c tLeft ( { 1, 3, 8 } ), tRight ( { 1, 8 } );
		MatchCandidateCursor_t tA, tB;
		tA.Reset(&tCandidates);
		tB.Reset(&tCandidates);
		EXPECT_EQ ( tA.Next(tLeft), 1U );
		tA.Hint(8);
		EXPECT_EQ ( tA.Next(tLeft), 8U );
		EXPECT_EQ ( tB.Next(tRight), 1U );
		EXPECT_EQ ( tB.Next(tRight), 8U );
	} );
}

// Noncandidate documents must never reach SeekHitlist(), even inside a leaf batch.
TEST ( MatchCandidates, RestrictBeforeReadingHits )
{
	Threads::CallCoroutine ( [] {
		MatchCandidates_t tCandidates;
		tCandidates.m_dRows.Add(1);
		tCandidates.m_dRows.Add(8);
		tCandidates.m_bActive = true;
		CandidateTestSetup_c tSetup;
		tSetup.m_pMatchCandidates = &tCandidates;
		XQLimitSpec_t tSpec;
		tSpec.m_dFieldMask.SetAll();
		XQNode_t tQuery(tSpec);
		auto * pWord = new CandidateTestWord_c ( { 0, 1, 2, 3, 8, 9 } );
		std::unique_ptr<ExtNode_i> pNode ( ExtNode_i::Create ( pWord, &tQuery, tSetup, false, false ) );
		pNode->SetCollectHits();
		const auto * pDocs = pNode->GetDocsChunk();
		ASSERT_NE ( pDocs, nullptr );
		EXPECT_EQ ( pDocs[0].m_tRowID, 1U );
		EXPECT_EQ ( pDocs[1].m_tRowID, 8U );
		EXPECT_EQ ( pDocs[2].m_tRowID, INVALID_ROWID );
		pNode->GetHits(pDocs);
		ASSERT_EQ ( pWord->m_dSought.GetLength(), 2 );
		EXPECT_EQ ( pWord->m_dSought[0], 1U );
		EXPECT_EQ ( pWord->m_dSought[1], 8U );
	} );
}

// Empty/exact-bound collections activate; overflow replays every original row.
TEST ( MatchCandidates, ReplayAtAndAboveLimit )
{
	Threads::CallCoroutine ( [] {
		for ( int iCount : { 0, 1, MatchCandidates_t::MAX_ROWS, MatchCandidates_t::MAX_ROWS+1, 100000 } )
		{
			MatchCandidates_t tCandidates;
			auto pIterator = BufferMatchCandidates ( std::make_unique<CandidateTestIterator_c>(iCount), tCandidates );
			EXPECT_EQ ( tCandidates.m_bActive, iCount<=MatchCandidates_t::MAX_ROWS );
			EXPECT_LE ( tCandidates.m_dRows.GetLength(), MatchCandidates_t::MAX_ROWS );
			int iExpected = 0;
			RowIdBlock_t dBlock;
			while ( pIterator->GetNextRowIdBlock(dBlock) )
				for ( RowID_t tRowID : dBlock )
					EXPECT_EQ ( tRowID, RowID_t(iExpected++) );
			EXPECT_EQ ( iExpected, iCount );
		}
	} );
}

/// Validate the global range and capture it once for a candidate context.
/// No inputs or return value; compare activation, overflow replay, and capacity release.
TEST ( MatchCandidates, ConfiguredLimitAndSnapshot )
{
	int iPrevious = GetMatchCandidatesLimit();
	AT_SCOPE_EXIT ( [iPrevious] { CSphString sError; SetMatchCandidatesLimit ( iPrevious, sError ); } );
	CSphString sError;
	EXPECT_FALSE ( SetMatchCandidatesLimit ( 0, sError ) );
	EXPECT_FALSE ( SetMatchCandidatesLimit ( MatchCandidates_t::MAX_CONFIGURED_ROWS+1, sError ) );
	EXPECT_EQ ( GetMatchCandidatesLimit(), iPrevious );
	ASSERT_TRUE ( SetMatchCandidatesLimit ( MatchCandidates_t::MAX_CONFIGURED_ROWS, sError ) );
	EXPECT_EQ ( GetMatchCandidatesLimit(), MatchCandidates_t::MAX_CONFIGURED_ROWS );
	ASSERT_TRUE ( SetMatchCandidatesLimit ( 100000, sError ) );
	MatchCandidates_t tCandidates;
	tCandidates.m_iMaxRows = GetMatchCandidatesLimit();
	ASSERT_TRUE ( SetMatchCandidatesLimit ( 1, sError ) );
	EXPECT_EQ ( tCandidates.m_iMaxRows, 100000 );
	Threads::CallCoroutine ( [&] {
		for ( int iCount : { 100000, 100001 } )
		{
			tCandidates.m_dRows.Resize(0);
			tCandidates.m_bActive = false;
			auto pIterator = BufferMatchCandidates ( std::make_unique<CandidateTestIterator_c>(iCount), tCandidates );
			EXPECT_EQ ( tCandidates.m_bActive, iCount==100000 );
			int iRows = 0;
			RowIdBlock_t dBlock;
			while ( pIterator->GetNextRowIdBlock(dBlock) )
				iRows += dBlock.GetLength();
			EXPECT_EQ ( iRows, iCount );
		}
		tCandidates.m_iMaxRows = MatchCandidates_t::MAX_CONFIGURED_ROWS;
		for ( int iCount : { MatchCandidates_t::MAX_CONFIGURED_ROWS, MatchCandidates_t::MAX_CONFIGURED_ROWS+1 } )
		{
			tCandidates.m_dRows.Resize(0);
			tCandidates.m_bActive = false;
			auto pIterator = BufferMatchCandidates ( std::make_unique<CandidateTestIterator_c>(iCount), tCandidates );
			EXPECT_EQ ( tCandidates.m_bActive, iCount==MatchCandidates_t::MAX_CONFIGURED_ROWS );
			EXPECT_LE ( tCandidates.m_dRows.AllocatedBytes(), int64_t(MatchCandidates_t::MAX_CONFIGURED_ROWS)*sizeof(RowID_t) );
			int iRows = 0;
			RowIdBlock_t dBlock;
			while ( pIterator->GetNextRowIdBlock(dBlock) )
				iRows += dBlock.GetLength();
			EXPECT_EQ ( iRows, iCount );
		}
		int iCapacity = tCandidates.m_dRows.GetLimit();
		tCandidates.m_dRows.Resize(0);
		EXPECT_EQ ( tCandidates.m_dRows.GetLimit(), iCapacity );
		tCandidates.m_dRows.Reset();
		EXPECT_EQ ( tCandidates.m_dRows.AllocatedBytes(), 0 );
	} );
}

// A hint crossing the materialized prefix must preserve the partially owned block.
TEST ( MatchCandidates, ReplayHintAcrossOverflowBlock )
{
	Threads::CallCoroutine ( [] {
		MatchCandidates_t tCandidates;
		auto pIterator = BufferMatchCandidates ( std::make_unique<CandidateTestIterator_c>(100000), tCandidates );
		ASSERT_FALSE(tCandidates.m_bActive);
		ASSERT_TRUE(pIterator->HintRowID(65536));
		int iExpected = 65536;
		RowIdBlock_t dBlock;
		while ( pIterator->GetNextRowIdBlock(dBlock) )
			for ( RowID_t tRowID : dBlock )
				EXPECT_EQ ( tRowID, RowID_t(iExpected++) );
		EXPECT_EQ ( iExpected, 100000 );
	} );
}

/// Parse hint combinations, quoted subkeys and facets; no inputs or return value.
/// Assert that nested lexer states preserve the hint and ordinary identifiers stay valid.
TEST ( MatchCandidates, HintGrammarAndFacets )
{
	InitParserOption();
	for ( const char * szSQL : {
		"SELECT id FROM r0 WHERE g=1 AND MATCH('a') /*+ MatchByCandidates */",
		"SELECT id FROM r0 WHERE id IN (1,2) AND MATCH('a') OPTION ranker=none /*+ matchbycandidates DocidIndex(id) */",
		"SELECT id FROM r0 /*+ SecondaryIndex(j.`key`,j.`other`) MatchByCandidates */",
		"SELECT id FROM r0 /*+ MatchByCandidates SecondaryIndex(j.`key`) MatchByCandidates */",
		"SELECT id FROM r0 /*+ SecondaryIndex(j.`key`) /* comment */ MatchByCandidates */",
		"SELECT id FROM r0 WHERE g=1 AND MATCH('a') /*+ SecondaryIndex(g) MatchByCandidates MatchByCandidates */ FACET g" } )
	{
		CSphVector<char> dSQL;
		dSQL.Resize(strlen(szSQL)+2);
		memcpy ( dSQL.Begin(), szSQL, strlen(szSQL)+1 );
		dSQL.Last() = 0;
		CSphVector<SqlStmt_t> dStmts;
		CSphString sError;
		ASSERT_TRUE ( sphParseSqlQuery ( { dSQL.Begin(), int(strlen(szSQL)) }, dStmts, sError, SPH_COLLATION_DEFAULT ) ) << sError.cstr();
		ASSERT_FALSE(dStmts.IsEmpty());
		for ( const auto & tStmt : dStmts )
			EXPECT_TRUE(tStmt.m_tQuery.m_bMatchByCandidates);
	}
	for ( const char * szSQL : {
		"SELECT id FROM r0 /*+ MatchByCandidates() */",
		"SELECT id FROM r0 /*+ MatchByCandidates(g) */" } )
	{
		CSphVector<char> dSQL;
		dSQL.Resize(strlen(szSQL)+2);
		memcpy ( dSQL.Begin(), szSQL, strlen(szSQL)+1 );
		dSQL.Last() = 0;
		CSphVector<SqlStmt_t> dStmts;
		CSphString sError;
		EXPECT_FALSE ( sphParseSqlQuery ( { dSQL.Begin(), int(strlen(szSQL)) }, dStmts, sError, SPH_COLLATION_DEFAULT ) );
	}
	char sIdentifier[] = "SELECT j.`key`,MatchByCandidates FROM r0 MatchByCandidates /* comment */ WHERE MatchByCandidates=1\0";
	CSphVector<SqlStmt_t> dIdentifierStmts;
	CSphString sIdentifierError;
	ASSERT_TRUE ( sphParseSqlQuery ( { sIdentifier, int(strlen(sIdentifier)) }, dIdentifierStmts, sIdentifierError, SPH_COLLATION_DEFAULT ) ) << sIdentifierError.cstr();
	EXPECT_FALSE(dIdentifierStmts[0].m_tQuery.m_bMatchByCandidates);
	CSphQuery tDefault;
	EXPECT_FALSE(tDefault.m_bMatchByCandidates);
}

/// Search real RAM rows while the user-variable hook changes its value on rebinding.
/// No inputs or return value; assert that hinted searches retain the original snapshot.
TEST_F ( RT, MatchCandidatesUservarSnapshot )
{
	Threads::CallCoroutine ( [&] {
		tCol.m_sName = "id";
		tCol.m_eAttrType = SPH_ATTR_BIGINT;
		tSrcSchema.AddAttr ( tCol, false );
		auto pDict = sphCreateDictionaryCRC ( tDictSettings, nullptr, pTok, "rt", false, 32, nullptr, sError );
		ASSERT_TRUE(pDict);
		auto pIndex = sphCreateIndexRT ( "testrt", RT_INDEX_FILE_NAME, tSrcSchema, 32*1024*1024 );
		pIndex->SetTokenizer ( pTok->Clone(SPH_CLONE_INDEX) );
		pIndex->SetDictionary ( pDict->Clone() );
		pIndex->PostSetup();
		StrVec_t dWarnings;
		ASSERT_TRUE ( pIndex->Prealloc ( false, nullptr, dWarnings ) );
		RtAccum_t tAcc;
		CSphString sFilter;
		for ( int i=1; i<=2; ++i )
		{
			InsertDocData_c tDoc(pIndex->GetMatchSchema());
			tDoc.SetID(i);
			tDoc.m_dFields.Fill ( { "cat", 3 } );
			ASSERT_TRUE ( pIndex->AddDocument ( tDoc, false, sFilter, sError, sWarning, &tAcc ) );
		}
		ASSERT_TRUE ( pIndex->Commit ( nullptr, &tAcc, &sError ) );

		ASSERT_FALSE(UservarsAvailable());
		static int iBindings;
		// Ignore the variable name; return {1} initially and {2} on any rebinding.
		SetUserVarsHook ( [] ( const CSphString & ) {
			UservarIntSet_c pValues { new UservarIntSetValues_c };
			pValues->Add ( ++iBindings==1 ? 1 : 2 );
			return pValues;
		} );
		AT_SCOPE_EXIT ( [] { SetUserVarsHook(nullptr); } );
		auto pParser = sphCreatePlainQueryParser();
		for ( int iShape=0; iShape<3; ++iShape )
			for ( bool bHint : { false, true } )
			{
				SCOPED_TRACE ( testing::Message() << "shape=" << iShape << ", hint=" << bHint );
				iBindings = 0;
				CSphQuery tQuery;
				tQuery.m_sQuery = "cat";
				tQuery.m_pQueryParser = pParser.get();
				tQuery.m_bMatchByCandidates = bHint;
				auto & tUservar = tQuery.m_dFilters.Add();
				tUservar.m_sAttrName = "id";
				tUservar.m_eType = SPH_FILTER_USERVAR;
				tUservar.m_dStrings.Add("ids");
				if ( iShape )
				{
					auto & tNative = tQuery.m_dFilters.Add();
					tNative.m_sAttrName = "id";
					tNative.m_eType = SPH_FILTER_VALUES;
					tNative.m_dValues.Add ( iShape==1 ? 1 : 3 );
				}
				if ( iShape==2 )
				{
					tQuery.m_dFilterTree.Add ( { -1, -1, 0, false } );
					tQuery.m_dFilterTree.Add ( { -1, -1, 1, false } );
					tQuery.m_dFilterTree.Add ( { 0, 1, -1, true } );
				}
				AggrResult_t tResult;
				CSphQueryResult tQueryResult;
				tQueryResult.m_pMeta = &tResult;
				SphQueueSettings_t tSettings(pIndex->GetMatchSchema());
				SphQueueRes_t tRes;
				std::unique_ptr<ISphMatchSorter> pSorter ( sphCreateQueue ( tSettings, tQuery, sError, tRes ) );
				ASSERT_TRUE(pSorter) << sError.cstr();
				auto * pQueue = pSorter.get();
				ASSERT_TRUE ( pIndex->MultiQuery ( tQueryResult, tQuery, { &pQueue, 1 }, CSphMultiQueryArgs(1) ) ) << tResult.m_sError.cstr();
				EXPECT_EQ ( iBindings, 1 );
				auto & tRows = tResult.m_dResults.Add();
				tRows.FillFromSorter(pQueue);
				ASSERT_EQ ( tResult.GetLength(), 1 );
				EXPECT_EQ ( tRows.m_dMatches[0].m_tRowID, 0U );
			}
	} );
}

/// Search a RAM row with/without the hint and profiler.
/// No inputs or return value; assert collection is profiled only for a hinted query.
TEST_F ( RT, MatchCandidatesRamProfileCollection )
{
	Threads::CallCoroutine ( [&] {
		tCol.m_sName = "id";
		tCol.m_eAttrType = SPH_ATTR_BIGINT;
		tSrcSchema.AddAttr ( tCol, false );
		auto pDict = sphCreateDictionaryCRC ( tDictSettings, nullptr, pTok, "rt", false, 32, nullptr, sError );
		ASSERT_TRUE ( pDict );
		auto pIndex = sphCreateIndexRT ( "testrt", RT_INDEX_FILE_NAME, tSrcSchema, 32*1024*1024 );
		pIndex->SetTokenizer ( pTok->Clone ( SPH_CLONE_INDEX ) );
		pIndex->SetDictionary ( pDict->Clone() );
		pIndex->PostSetup();
		StrVec_t dWarnings;
		ASSERT_TRUE ( pIndex->Prealloc ( false, nullptr, dWarnings ) );
		InsertDocData_c tDoc ( pIndex->GetMatchSchema() );
		tDoc.SetID ( 1 );
		tDoc.m_dFields.Fill ( { "cat", 3 } );
		RtAccum_t tAcc;
		CSphString sFilter;
		ASSERT_TRUE ( pIndex->AddDocument ( tDoc, false, sFilter, sError, sWarning, &tAcc ) );
		ASSERT_TRUE ( pIndex->Commit ( nullptr, &tAcc, &sError ) );

		auto pParser = sphCreatePlainQueryParser();
		for ( int iMode=0; iMode<3; ++iMode )
		{
			SCOPED_TRACE ( testing::Message() << "mode=" << iMode );
			CSphQuery tQuery;
			tQuery.m_sQuery = "cat";
			tQuery.m_pQueryParser = pParser.get();
			tQuery.m_bMatchByCandidates = iMode!=0;
			auto & tFilter = tQuery.m_dFilters.Add();
			tFilter.m_sAttrName = "id";
			tFilter.m_eType = SPH_FILTER_VALUES;
			tFilter.m_dValues.Add ( 1 );

			QueryProfile_c tProfile;
			tProfile.Start ( SPH_QSTATE_UNKNOWN );
			AggrResult_t tResult;
			tResult.m_pProfile = iMode==2 ? nullptr : &tProfile;
			CSphQueryResult tQueryResult;
			tQueryResult.m_pMeta = &tResult;
			SphQueueSettings_t tSettings ( pIndex->GetMatchSchema() );
			SphQueueRes_t tRes;
			std::unique_ptr<ISphMatchSorter> pSorter ( sphCreateQueue ( tSettings, tQuery, sError, tRes ) );
			ASSERT_TRUE ( pSorter ) << sError.cstr();
			auto * pQueue = pSorter.get();
			ASSERT_TRUE ( pIndex->MultiQuery ( tQueryResult, tQuery, { &pQueue, 1 }, CSphMultiQueryArgs(1) ) ) << tResult.m_sError.cstr();
			EXPECT_EQ ( pQueue->GetLength(), 1 );
			EXPECT_EQ ( tProfile.m_dSwitches[SPH_QSTATE_CANDIDATE_COLLECT]>0, iMode==1 );
			if ( iMode )
			{
				ASSERT_FALSE ( tResult.m_tIteratorStats.m_dIterators.IsEmpty() );
				EXPECT_STREQ ( tResult.m_tIteratorStats.m_dIterators[0].m_sType.cstr(), "MatchByCandidates" );
			}
		}
	} );
}

/// Merge two old-agent acknowledgments and an upgraded nested result.
/// No inputs or return value; assert unique descriptions and correct usage totals.
TEST ( MatchCandidates, NestedAgentDescriptions )
{
	CSphQueryResultMeta tOld, tNew, tNested;
	DescribeMatchCandidates ( tOld, "agent_not_acknowledged" );
	DescribeMatchCandidates ( tOld, "agent_not_acknowledged" );
	ASSERT_EQ ( tOld.m_tIteratorStats.m_dIterators.GetLength(), 1 );
	EXPECT_EQ ( tOld.m_tIteratorStats.m_iTotal, 2 );
	EXPECT_EQ ( tOld.m_tIteratorStats.m_dIterators[0].m_iUsed, 2 );
	DescribeMatchCandidates(tNew);
	tNested.m_tIteratorStats.Merge(tOld.m_tIteratorStats);
	tNested.m_tIteratorStats.Merge(tNew.m_tIteratorStats);
	tNested.m_tIteratorStats.Merge(tOld.m_tIteratorStats);
	ASSERT_EQ ( tNested.m_tIteratorStats.m_dIterators.GetLength(), 2 );
	EXPECT_EQ ( tNested.m_tIteratorStats.m_iTotal, 5 );
	EXPECT_EQ ( tNested.m_tIteratorStats.m_dIterators[0].m_iUsed, 4 );
	EXPECT_EQ ( tNested.m_tIteratorStats.m_dIterators[1].m_iUsed, 1 );
}

// A leaf can bind before collection, then handle an empty set or a segment reset.
TEST ( MatchCandidates, EmptyAndLateActivation )
{
	Threads::CallCoroutine ( [] {
		MatchCandidates_t tCandidates;
		MatchCandidateCursor_t tCursor;
		tCursor.Reset(&tCandidates);
		EXPECT_FALSE(tCursor.Active());
		tCandidates.m_bActive = true;
		CandidateTestWord_c tWord ( { 1, 3 } );
		EXPECT_EQ ( tCursor.Next(tWord), INVALID_ROWID );
		tCandidates.m_dRows.Add(3);
		tCursor.Reset(&tCandidates);
		EXPECT_EQ ( tCursor.Next(tWord), 3U );
	} );
}

// Once aborted, the cursor must not consume even one more underlying posting.
TEST ( MatchCandidates, CancellationDoesNotConsumeAnotherPosting )
{
	Threads::CallCoroutine ( [] {
		MatchCandidates_t tCandidates;
		tCandidates.m_dRows.Add(0);
		tCandidates.m_dRows.Add(2);
		tCandidates.m_bActive = true;
		CandidateTestWord_c tWord ( { 0, 1, 2 } );
		MatchCandidateCursor_t tCursor;
		tCursor.Reset(&tCandidates);
		EXPECT_EQ ( tCursor.Next(tWord), 0U );
		tCandidates.m_bAborted = true;
		EXPECT_EQ ( tCursor.Next(tWord), INVALID_ROWID );
		EXPECT_EQ ( tWord.GetNextDoc().m_tRowID, 1U );
	} );
}

// Real timeout/session cancellation must set sticky abort state and a useful warning.
TEST ( MatchCandidates, TimeoutAndSessionCancellation )
{
	Threads::CallCoroutine ( [] {
		CSphString sWarning;
		MatchCandidates_t tExpired;
		tExpired.m_iMaxTimer = 1; // before the timer's last tracked timestamp, without sleeping
		tExpired.m_pWarning = &sWarning;
		EXPECT_FALSE(tExpired.Check());
		EXPECT_TRUE(tExpired.m_bAborted);
		EXPECT_STREQ ( sWarning.cstr(), "query time exceeded max_query_time" );
		EXPECT_FALSE(tExpired.Check());

		MatchCandidates_t tKilled;
		tKilled.m_pWarning = &sWarning;
		bool bWasKilled = session::GetKilled();
		session::SetKilled(true);
		bool bContinue = tKilled.Check();
		session::SetKilled(bWasKilled);
		EXPECT_FALSE(bContinue);
		EXPECT_TRUE(tKilled.m_bAborted);
		EXPECT_STREQ ( sWarning.cstr(), "query was killed" );
	} );
}
