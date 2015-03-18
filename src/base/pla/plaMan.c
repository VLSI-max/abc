/**CFile****************************************************************

  FileName    [plaMan.c]

  SystemName  [ABC: Logic synthesis and verification system.]

  PackageName [SOP manager.]

  Synopsis    [Scalable SOP transformations.]

  Author      [Alan Mishchenko]
  
  Affiliation [UC Berkeley]

  Date        [Ver. 1.0. Started - March 18, 2015.]

  Revision    [$Id: plaMan.c,v 1.00 2014/09/12 00:00:00 alanmi Exp $]

***********************************************************************/

#include "pla.h"

ABC_NAMESPACE_IMPL_START

////////////////////////////////////////////////////////////////////////
///                        DECLARATIONS                              ///
////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////
///                     FUNCTION DEFINITIONS                         ///
////////////////////////////////////////////////////////////////////////

/**Function*************************************************************

  Synopsis    [Generates prime detector for the given bit-widths.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
Vec_Int_t * Pla_GenPrimes( int nVars )
{
    int i, n, nBits = ( 1 << nVars );
    Vec_Bit_t * vMap = Vec_BitStart( nBits );
    Vec_Int_t * vPrimes = Vec_IntAlloc( 1000 );
    Vec_BitWriteEntry(vMap, 0, 1);
    Vec_BitWriteEntry(vMap, 1, 1);
    for ( n = 2; n < nBits; n++ )
        if ( !Vec_BitEntry(vMap, n) )
            for ( i = 2*n; i < nBits; i += n )
                Vec_BitWriteEntry(vMap, i, 1);
    for ( n = 2; n < nBits; n++ )
        if ( !Vec_BitEntry(vMap, n) )
            Vec_IntPush( vPrimes, n );
    printf( "Primes up to 2^%d = %d\n", nVars, Vec_IntSize(vPrimes) );
//    Abc_GenCountHits1( vMap, vPrimes, nVars );
    Vec_BitFree( vMap );
    return vPrimes;
}
Pla_Man_t * Pla_GenFromMinterms( char * pName, Vec_Int_t * vMints, int nVars )
{
    Pla_Man_t * p = Pla_ManAlloc( pName, nVars, 1, Vec_IntSize(vMints) );
    int i, k, Lit, Mint;
    word * pCube;
    Pla_ForEachCubeIn( p, pCube, i )
    {
        Mint = Vec_IntEntry(vMints, i);
        Pla_CubeForEachLitIn( p, pCube, Lit, k )
            Pla_CubeSetLit( pCube, k, ((Mint >> k) & 1) ? PLA_LIT_ONE : PLA_LIT_ZERO );
    }
    Pla_ForEachCubeOut( p, pCube, i )
        Pla_CubeSetLit( pCube, 0, PLA_LIT_ONE );
    return p;
}
Pla_Man_t * Pla_ManPrimeDetector( int nVars )
{
    char pName[1000];
    Pla_Man_t * p;
    Vec_Int_t * vMints = Pla_GenPrimes( nVars );
    sprintf( pName, "primes%02d", nVars );
    p = Pla_GenFromMinterms( pName, vMints, nVars );
    Vec_IntFree( vMints );
    return p;
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
Vec_Bit_t * Pla_GenRandom( int nVars, int nNums, int fNonZero )
{
    int Mint, Count = 0;
    Vec_Bit_t * vBits = Vec_BitStart( 1 << nVars );
    assert( nVars > 0 && nVars <= 30 );
    assert( nNums > 0 && nNums < (1 << (nVars - 1)) );
    while ( Count < nNums )
    {
        Mint = Gia_ManRandom(0) & ((1 << nVars) - 1);
        if ( fNonZero && Mint == 0 )
            continue;
        if ( Vec_BitEntry(vBits, Mint) )
            continue;
        Vec_BitWriteEntry( vBits, Mint, 1 );
        Count++;
    }
    return vBits;
}
Pla_Man_t * Pla_ManGenerate( int nInputs, int nOutputs, int nCubes, int fVerbose )
{
    Vec_Bit_t * vBits;
    int i, k, Count;
    word * pCube;
    Pla_Man_t * p = Pla_ManAlloc( "rand", nInputs, nOutputs, nCubes );
    // generate nCube random input minterms
    vBits = Pla_GenRandom( nInputs, nCubes, 0 );
    for ( i = Count = 0; i < Vec_BitSize(vBits); i++ )
        if ( Vec_BitEntry(vBits, i) )
        {
            pCube = Pla_CubeIn( p, Count++ );
            for ( k = 0; k < nInputs; k++ )
                Pla_CubeSetLit( pCube, k, ((i >> k) & 1) ? PLA_LIT_ONE : PLA_LIT_ZERO );
        }
    assert( Count == nCubes );
    Vec_BitFree( vBits );
    // generate nCube random output minterms
    if ( nOutputs > 1 )
    {
        vBits = Pla_GenRandom( nOutputs, nCubes, 1 );
        for ( i = Count = 0; i < Vec_BitSize(vBits); i++ )
            if ( Vec_BitEntry(vBits, i) )
            {
                pCube = Pla_CubeOut( p, Count++ );
                for ( k = 0; k < nOutputs; k++ )
                    Pla_CubeSetLit( pCube, k, ((i >> k) & 1) ? PLA_LIT_ONE : PLA_LIT_ZERO );
            }
        assert( Count == nCubes );
        Vec_BitFree( vBits );
    }
    else
    {
        Pla_ForEachCubeOut( p, pCube, i )
            Pla_CubeSetLit( pCube, 0, PLA_LIT_ONE );
    }
    return p;
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Pla_ManConvertFromBits( Pla_Man_t * p )
{
    word * pCube; int i, k, Lit;
    Vec_WecClear( &p->vLits );
    Vec_WecClear( &p->vOccurs );
    Vec_WecInit( &p->vLits, Pla_ManCubeNum(p) );
    Vec_WecInit( &p->vOccurs, 2*Pla_ManInNum(p) );
    Pla_ForEachCubeIn( p, pCube, i )
        Pla_CubeForEachLitIn( p, pCube, Lit, k )
            if ( Lit != PLA_LIT_DASH )
            {
                Lit = Abc_Var2Lit( k, Lit == PLA_LIT_ZERO );
                Vec_WecPush( &p->vLits,   i, Lit );
                Vec_WecPush( &p->vOccurs, Lit, i );
            }
}
void Pla_ManConvertToBits( Pla_Man_t * p )
{
    Vec_Int_t * vCube; int i, k, Lit;
    Vec_IntFillNatural( &p->vCubes, Vec_WecSize(&p->vLits) );
    Vec_WrdFill( &p->vInBits,  Pla_ManCubeNum(p) * p->nInWords,  0 );
    Vec_WecForEachLevel( &p->vLits, vCube, i )
        Vec_IntForEachEntry( vCube, Lit, k )
            Pla_CubeSetLit( Pla_CubeIn(p, i), Abc_Lit2Var(Lit), Abc_LitIsCompl(Lit) ? PLA_LIT_ZERO : PLA_LIT_ONE );
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
int Pla_ManDist1Num( Pla_Man_t * p )
{
    word * pCube1, * pCube2; 
    int i, k, Dist, Count = 0;
    Pla_ForEachCubeIn( p, pCube1, i )
    Pla_ForEachCubeInStart( p, pCube2, k, i+1 )
    {
        Dist = Pla_CubesAreDistance1( pCube1, pCube2, p->nInWords );
//        Dist = Pla_CubesAreConsensus( pCube1, pCube2, p->nInWords, NULL );
        Count += (Dist == 1);
    }
    return Count;
}
int Pla_ManDist1NumTest( Pla_Man_t * p )
{
    abctime clk = Abc_Clock();
    int Count = Pla_ManDist1Num( p );
    printf( "Found %d pairs among %d cubes using cube pair enumeration.  ", Count, Pla_ManCubeNum(p) );
    Abc_PrintTime( 1, "Time", Abc_Clock() - clk );
    return 1;
}

////////////////////////////////////////////////////////////////////////
///                       END OF FILE                                ///
////////////////////////////////////////////////////////////////////////


ABC_NAMESPACE_IMPL_END

