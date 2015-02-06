/**CFile****************************************************************

  FileName    [cbaCba.c]

  SystemName  [ABC: Logic synthesis and verification system.]

  PackageName [Hierarchical word-level netlist.]

  Synopsis    [Verilog parser.]

  Author      [Alan Mishchenko]
  
  Affiliation [UC Berkeley]

  Date        [Ver. 1.0. Started - November 29, 2014.]

  Revision    [$Id: cbaCba.c,v 1.00 2014/11/29 00:00:00 alanmi Exp $]

***********************************************************************/

#include "cba.h"

ABC_NAMESPACE_IMPL_START

////////////////////////////////////////////////////////////////////////
///                        DECLARATIONS                              ///
////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////
///                     FUNCTION DEFINITIONS                         ///
////////////////////////////////////////////////////////////////////////


/**Function*************************************************************

  Synopsis    [Read CBA.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
int CbaManReadCbaLine( Vec_Str_t * vOut, int * pPos, char * pBuffer, char * pLimit )
{
    char c; 
    while ( (c = Vec_StrEntry(vOut, (*pPos)++)) != '\n' && pBuffer < pLimit )
        *pBuffer++ = c;
    *pBuffer = 0;
    return pBuffer < pLimit;
}
int CbaManReadCbaNameAndNums( char * pBuffer, int * Num1, int * Num2, int * Num3 )
{
    *Num1 = *Num2 = *Num3 = -1;
    // read name
    while ( *pBuffer && *pBuffer != ' ' )
        pBuffer++;
    if ( !*pBuffer )
        return 0;
    assert( *pBuffer == ' ' );
    *pBuffer = 0;
    // read Num1
    *Num1 = atoi(++pBuffer);
    while ( *pBuffer && *pBuffer != ' ' )
        pBuffer++;
    if ( !*pBuffer )
        return 0;
    // read Num2
    assert( *pBuffer == ' ' );
    *Num2 = atoi(++pBuffer);
    while ( *pBuffer && *pBuffer != ' ' )
        pBuffer++;
    if ( !*pBuffer )
        return 1;
    // read Num3
    assert( *pBuffer == ' ' );
    *Num3 = atoi(++pBuffer);
    return 1;
}
void Cba_ManReadCbaVecStr( Vec_Str_t * vOut, int * pPos, Vec_Str_t * p, int nSize )
{
    memcpy( Vec_StrArray(p), Vec_StrArray(vOut) + *pPos, nSize );
    *pPos += nSize;
    p->nSize = nSize;
    assert( Vec_StrSize(p) == Vec_StrCap(p) );
}
void Cba_ManReadCbaVecInt( Vec_Str_t * vOut, int * pPos, Vec_Int_t * p, int nSize )
{
    memcpy( Vec_IntArray(p), Vec_StrArray(vOut) + *pPos, nSize );
    *pPos += nSize;
    p->nSize = nSize / 4;
    assert( Vec_IntSize(p) == Vec_IntCap(p) );
}
void Cba_ManReadCbaNtk( Vec_Str_t * vOut, int * pPos, Cba_Ntk_t * pNtk )
{
    Cba_ManReadCbaVecInt( vOut, pPos, &pNtk->vInputs,  4 * Cba_NtkPiNumAlloc(pNtk) );
    Cba_ManReadCbaVecInt( vOut, pPos, &pNtk->vOutputs, 4 * Cba_NtkPoNumAlloc(pNtk) );
    Cba_ManReadCbaVecStr( vOut, pPos, &pNtk->vType,        Cba_NtkObjNumAlloc(pNtk) );
    Cba_ManReadCbaVecInt( vOut, pPos, &pNtk->vIndex,   4 * Cba_NtkObjNumAlloc(pNtk) );
    Cba_ManReadCbaVecInt( vOut, pPos, &pNtk->vFanin,   4 * Cba_NtkObjNumAlloc(pNtk) );
}
Cba_Man_t * Cba_ManReadCbaInt( Vec_Str_t * vOut )
{
    Cba_Man_t * p;
    Cba_Ntk_t * pNtk;
    char Buffer[1000] = "#"; 
    int i, NameId, Pos = 0, Num1, Num2, Num3;
    while ( Buffer[0] == '#' )
        if ( !CbaManReadCbaLine(vOut, &Pos, Buffer, Buffer+1000) )
            return NULL;
    if ( !CbaManReadCbaNameAndNums(Buffer, &Num1, &Num2, &Num3) )
        return NULL;
    // start manager
    assert( Num1 > 0 && Num2 >= 0 );
    p = Cba_ManAlloc( Buffer, Num1 );
    Vec_IntGrow( &p->vInfo, 4 * Num2 );
    // start networks
    Cba_ManForEachNtk( p, pNtk, i )
    {
        if ( !CbaManReadCbaLine(vOut, &Pos, Buffer, Buffer+1000) )
        {
            Cba_ManFree( p );
            return NULL;
        }
        if ( !CbaManReadCbaNameAndNums(Buffer, &Num1, &Num2, &Num3) )
        {
            Cba_ManFree( p );
            return NULL;
        }
        assert( Num1 > 0 && Num2 > 0 && Num3 > 0 );
        NameId = Abc_NamStrFindOrAdd( p->pStrs, Buffer, NULL );
        Cba_NtkAlloc( pNtk, NameId, Num1, Num2, Num3 );
    }
    // read networks
    Cba_ManForEachNtk( p, pNtk, i )
        Cba_ManReadCbaNtk( vOut, &Pos, pNtk );
    Cba_ManReadCbaVecInt( vOut, &Pos, &p->vInfo,  4 * Vec_IntSize(&p->vInfo) );
    assert( Pos == Vec_StrSize(vOut) );
    return p;
}
Cba_Man_t * Cba_ManReadCba( char * pFileName )
{
    Cba_Man_t * p;
    FILE * pFile;
    Vec_Str_t * vOut;
    int nFileSize;
    pFile = fopen( pFileName, "rb" );
    if ( pFile == NULL )
    {
        printf( "Cannot open file \"%s\" for reading.\n", pFileName );
        return NULL;
    }
    // get the file size, in bytes
    fseek( pFile, 0, SEEK_END );  
    nFileSize = ftell( pFile );  
    rewind( pFile ); 
    // load the contents
    vOut = Vec_StrAlloc( nFileSize );
    vOut->nSize = vOut->nCap;
    assert( nFileSize == Vec_StrSize(vOut) );
    nFileSize = fread( Vec_StrArray(vOut), 1, Vec_StrSize(vOut), pFile );
    assert( nFileSize == Vec_StrSize(vOut) );
    fclose( pFile );
    // read the library
    p = Cba_ManReadCbaInt( vOut );
    if ( p != NULL )
    {
        ABC_FREE( p->pSpec );
        p->pSpec = Abc_UtilStrsav( pFileName );
    }
    Vec_StrFree( vOut );
    return p;
}

/**Function*************************************************************

  Synopsis    [Write CBA.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Cba_ManWriteCbaNtk( Vec_Str_t * vOut, Cba_Ntk_t * pNtk )
{
    Vec_StrPushBuffer( vOut, (char *)Vec_IntArray(&pNtk->vInputs),  4 * Cba_NtkPiNum(pNtk) );
    Vec_StrPushBuffer( vOut, (char *)Vec_IntArray(&pNtk->vOutputs), 4 * Cba_NtkPoNum(pNtk) );
    Vec_StrPushBuffer( vOut, (char *)Vec_StrArray(&pNtk->vType),        Cba_NtkObjNum(pNtk) );
    Vec_StrPushBuffer( vOut, (char *)Vec_IntArray(&pNtk->vIndex),   4 * Cba_NtkObjNum(pNtk) );
    Vec_StrPushBuffer( vOut, (char *)Vec_IntArray(&pNtk->vFanin),   4 * Cba_NtkObjNum(pNtk) );
}
void Cba_ManWriteCbaInt( Vec_Str_t * vOut, Cba_Man_t * p )
{
    char Buffer[1000];
    Cba_Ntk_t * pNtk; int i;
    sprintf( Buffer, "# Design \"%s\" written by ABC on %s\n", Cba_ManName(p), Extra_TimeStamp() );
    Vec_StrPrintStr( vOut, Buffer );
    // write short info
    sprintf( Buffer, "%s %d %d \n", Cba_ManName(p), Cba_ManNtkNum(p), Cba_ManInfoNum(p) );
    Vec_StrPrintStr( vOut, Buffer );
    Cba_ManForEachNtk( p, pNtk, i )
    {
        sprintf( Buffer, "%s %d %d %d \n", Cba_NtkName(pNtk), Cba_NtkPiNum(pNtk), Cba_NtkPoNum(pNtk), Cba_NtkObjNum(pNtk) );
        Vec_StrPrintStr( vOut, Buffer );
    }
    Cba_ManForEachNtk( p, pNtk, i )
        Cba_ManWriteCbaNtk( vOut, pNtk );
    Vec_StrPushBuffer( vOut, (char *)Vec_IntArray(&p->vInfo), 16 * Cba_ManInfoNum(p) );
}
void Cba_ManWriteCba( char * pFileName, Cba_Man_t * p )
{
    Vec_Str_t * vOut;
    assert( p->pMioLib == NULL );
    vOut = Vec_StrAlloc( 10000 );
    Cba_ManWriteCbaInt( vOut, p );
    if ( Vec_StrSize(vOut) > 0 )
    {
        FILE * pFile = fopen( pFileName, "wb" );
        if ( pFile == NULL )
            printf( "Cannot open file \"%s\" for writing.\n", pFileName );
        else
        {
            fwrite( Vec_StrArray(vOut), 1, Vec_StrSize(vOut), pFile );
            fclose( pFile );
        }
    }
    Vec_StrFree( vOut );    
}

////////////////////////////////////////////////////////////////////////
///                       END OF FILE                                ///
////////////////////////////////////////////////////////////////////////


ABC_NAMESPACE_IMPL_END

