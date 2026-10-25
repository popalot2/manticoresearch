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

#ifndef _sphinxsearch_
#define _sphinxsearch_

#include "sphinxquery/sphinxquery.h"
#include "sphinxint.h"

//////////////////////////////////////////////////////////////////////////

/// term modifiers
enum TermPosFilter_e
{
	TERM_POS_NONE = 0,
	TERM_POS_FIELD_LIMIT = 1,
	TERM_POS_FIELD_START = 2,
	TERM_POS_FIELD_END = 3,
	TERM_POS_FIELD_STARTEND = 4,
	TERM_POS_ZONES = 5,
};


/// decoder state saved at a certain offset
struct SkiplistEntry_t
{
	RowID_t		m_tBaseRowIDPlus1;	///< delta decoder rowid base (stored as base rowid + 1)
	int64_t		m_iOffset;			///< offset in the doclist file (relative to the doclist start)
	int64_t		m_iBaseHitlistPos;	///< delta decoder hitlist offset base
};

bool operator < ( const SkiplistEntry_t & a, RowID_t b );
bool operator == ( const SkiplistEntry_t & a, RowID_t b );
bool operator < ( RowID_t a, const SkiplistEntry_t & b );

struct DictEntry_t;
struct SkipData_t
{
	CSphVector<SkiplistEntry_t> m_dSkiplist;

	void Read ( const BYTE * pSkips, const DictEntry_t & tRes, int iDocs, int iSkipBlockSize );
};

class RtIndex_c;
struct RtGuard_t;
struct MatchCandidates_t;

/// term, searcher view
class ISphQword
{
public:
	// setup by query parser
	CSphString		m_sWord;		///< my copy of word
	CSphString		m_sDictWord;	///< word after being processed by dict (eg. stemmed)
	SphWordID_t		m_uWordID = 0;	///< word ID, from dictionary
	TermPosFilter_e	m_iTermPos = TERM_POS_NONE;
	int				m_iAtomPos = 0;	///< word position, from query
	float			m_fBoost = 1.0f;///< IDF keyword boost (multiplier)
	bool			m_bExpanded = false;	///< added by prefix expansion
	bool			m_bExcluded = false;	///< excluded by the query (rval to operator NOT)

	// setup by QwordSetup()
	int				m_iDocs = 0;	///< document count, from wordlist
	int				m_iHits = 0;	///< hit count, from wordlist
	bool			m_bHasHitlist = true;	///< hitlist presence flag

	SkipData_t *	m_pSkipData = nullptr;
	bool			m_bSkipFromCache = false;

	// iterator state
	FieldMask_t		m_dQwordFields;	///< current match fields
	DWORD			m_uMatchHits = 0;	///< current match hits count
	SphOffset_t		m_iHitlistPos = 0;	///< current position in hitlist, from doclist

protected:
	bool			m_bAllFieldsKnown = false; ///< whether the all match fields is known, or only low 32.

public:
	ISphQword ()
	{
		m_dQwordFields.UnsetAll();
	}

	virtual ~ISphQword ()
	{
		assert ( !m_bSkipFromCache );
		SafeDelete(m_pSkipData);
	}

	virtual RowID_t				AdvanceTo ( RowID_t tRowID );
	virtual bool				HintRowID ( RowID_t ) { return false; }
	virtual const CSphMatch &	GetNextDoc() = 0;
	virtual void				SeekHitlist ( SphOffset_t uOff ) = 0;
	virtual Hitpos_t			GetNextHit () = 0;
	virtual void				CollectHitMask ();
	virtual void				Reset();

	int							GetAtomPos() const;

	virtual bool SetupScan ( const RtIndex_c * pIndex, int iSegment, const RtGuard_t& tGuard ) { return false; }
};


/// term setup, searcher view
class CSphQueryNodeCache;
class ISphZoneCheck;
struct CSphQueryStats;
class ISphQwordSetup : ISphNoncopyable
{
public:
	const CSphIndex *		m_pIndex		{nullptr};
	int						m_iDynamicRowitems {0};
	int64_t					m_iMaxTimer		{0};
	CSphString *			m_pWarning		{nullptr};
	CSphQueryContext *		m_pCtx			{nullptr};
	CSphQueryNodeCache *	m_pNodeCache	{nullptr};
	mutable ISphZoneCheck *	m_pZoneChecker	{nullptr};
	CSphQueryStats *		m_pStats		{nullptr};
	mutable bool			m_bSetQposMask	{false};
	DictRefPtr_c			m_pDict;
	mutable KeywordBuf_t	m_tKeywordBuf;
	bool					m_bHasWideFields { false };
	MatchCandidates_t *		m_pMatchCandidates { nullptr }; ///< borrowed opt-in state; null leaves normal matching intact

	virtual ~ISphQwordSetup () {}

	virtual ISphQword *					QwordSpawn ( const XQKeyword_t & tWord ) const = 0;
	virtual bool						QwordSetup ( ISphQword * pQword ) const = 0;
	inline void SetDict ( DictRefPtr_c pDict )
	{
		m_pDict = std::move ( pDict );
		assert ( m_pDict );
		m_tKeywordBuf.Reset ( m_pDict->GetSettings().GetDictFormat() );
	}
	inline DictRefPtr_c Dict() const { return m_pDict; }

	virtual ISphQword *					ScanSpawn ( int iAtomPos ) const = 0;
};

/// generic ranker interface
class ISphRanker : public ISphExtra
{
public:
	virtual CSphMatch *			GetMatchesBuffer() = 0;
	virtual int					GetMatches () = 0;
	virtual void				Reset ( const ISphQwordSetup & tSetup ) = 0;
	virtual bool				IsCache() const { return false; }
	virtual void				FinalizeCache ( const ISphSchema & ) {}

	virtual NodeEstimate_t		Estimate ( int64_t iTotalDocs ) const = 0;
};

/// factory
std::unique_ptr<ISphRanker> sphCreateRanker ( const XQQuery_t & tXQ, const CSphQuery & tQuery, const QueryExecutionSettings_t & tQuerySettings, CSphQueryResultMeta & tMeta, const ISphQwordSetup & tTermSetup, const CSphQueryContext & tCtx, const ISphSchema & tSorterSchema );

class QwordScan_c : public ISphQword
{
public:
	QwordScan_c ( int iRowsCount, int iAtomPos, int iFieldsCount );

	const CSphMatch & GetNextDoc() final;

	void SeekHitlist ( SphOffset_t uOff ) override {}
	Hitpos_t GetNextHit () override { return EMPTY_HIT; }
	void Reset () final;

protected:
	RowID_t m_iRowsCount = INVALID_ROWID;
	bool m_bDone = false;
	CSphMatch m_tDoc;
	int m_iFieldsCount;

	virtual bool IsAliveRow (  RowID_t ) const { return true; }
	void SetFieldMask();
};


//////////////////////////////////////////////////////////////////////////

/// hit mark, used for snippets generation
struct SphHitMark_t
{
	DWORD	m_uPosition;
	DWORD	m_uSpan;

	bool operator == ( const SphHitMark_t & rhs ) const
	{
		return m_uPosition==rhs.m_uPosition && m_uSpan==rhs.m_uSpan;
	}
};

/// hit marker, used for snippets generation
class CSphHitMarker
{
public:
	class ExtNode_i *		m_pRoot;

public:
							CSphHitMarker() : m_pRoot ( NULL ) {}
							~CSphHitMarker();

	void					Mark ( CSphVector<SphHitMark_t> & );
	static CSphHitMarker *	Create ( const XQNode_t * pRoot, const ISphQwordSetup & tSetup );
};

//////////////////////////////////////////////////////////////////////////

CSphString sphXQNodeToStr ( const XQNode_t * pNode );
Bson_t sphExplainQuery ( const XQNode_t * pNode, const CSphSchema * pSchema = nullptr, const StrVec_t * pZones = nullptr );

namespace sph
{
	void RenderBsonPlan ( StringBuilder_c& tRes, const bson::NodeHandle_t & dBson, bool bDot );
	CSphString RenderBsonPlanBrief ( const bson::NodeHandle_t & dBson );
}

struct ExplainQueryArgs_t
{
	const char *		m_szQuery = nullptr;
	const CSphSchema *	m_pSchema = nullptr;
	DictRefPtr_c		m_pDict;
	std::unique_ptr<ISphFieldFilter> m_pFieldFilter;
	const CSphIndexSettings * m_pSettings = nullptr;
	TokenizerRefPtr_c m_pQueryTokenizer;
	const ISphWordlist * m_pWordlist = nullptr;
	int m_iExpandKeywords = 0;
	int m_iExpansionLimit = 0;
	bool m_bExpandPrefix = false;
	cRefCountedRefPtrGeneric_t m_pIndexData;
	const CSphBitvec * m_pMorphFields { nullptr };
};

Bson_t Explain ( ExplainQueryArgs_t & tArgs );

class WordlistStub_c : public ISphWordlist
{
public:
	WordlistStub_c() {}
	void GetPrefixedWords ( const char * , int , const char * , ISphWordlist::Args_t & ) const override {}
	void GetInfixedWords ( const char * , int , const char * , ISphWordlist::Args_t & ) const override {}
	void ScanRegexWords ( const VecTraits_T<RegexTerm_t> & dTerms, const ISphWordlist::Args_t & tArgs, const VecExpandConv_t & dConverters ) const override {}

};

// Query-local candidate rows are sorted, unique and immutable during matching.
// Row IDs are chunk-local; only a completely collected set may become active.
struct MatchCandidates_t
{
	static constexpr int MAX_ROWS = 65536;
	static constexpr int MAX_CONFIGURED_ROWS = 524288;
	CSphVector<RowID_t> m_dRows;
	int m_iMaxRows = MAX_ROWS; ///< limit sampled once for this search context
	bool m_bActive = false; ///< false on unsupported/oversized collection: use normal execution
	bool m_bAborted = false; ///< sticky interruption; never treat a partial prefix as complete
	bool m_bSequential = false; ///< RAM postings have no skip checkpoints
	int64_t m_iMaxTimer = 0;
	CSphString * m_pWarning = nullptr;

	bool Check();
};

/// Read the per-daemon candidate limit; no parameters, returns a row count.
int GetMatchCandidatesLimit();
/// Set the per-daemon limit from iRows; return false and fill sError when out of range.
bool SetMatchCandidatesLimit ( int64_t iRows, CSphString & sError );

// Independent posting/candidate cursor for each leaf, including RAM readers that
// cannot return their current posting from AdvanceTo().
struct MatchCandidateCursor_t
{
	MatchCandidates_t * m_pCandidates = nullptr;
	int m_iNext = 0;
	RowID_t m_tPosting = INVALID_ROWID;

	void Reset ( MatchCandidates_t * pCandidates );
	bool Active() const;
	void Hint ( RowID_t tRowID );
	RowID_t Next ( ISphQword & tWord, RowID_t tMinRowID = 0 );
};

class RowidIterator_i;
std::unique_ptr<RowidIterator_i> BufferMatchCandidates ( std::unique_ptr<RowidIterator_i> pIterator, MatchCandidates_t & tCandidates );
void DescribeMatchCandidates ( CSphQueryResultMeta & tMeta, const char * sFallback = nullptr );

#endif // _sphinxsearch_
