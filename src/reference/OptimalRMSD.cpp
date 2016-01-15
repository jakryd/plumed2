/* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
   Copyright (c) 2013-2016 The plumed team
   (see the PEOPLE file at the root of the distribution for a list of names)

   See http://www.plumed.org for more information.

   This file is part of plumed, version 2.

   plumed is free software: you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   plumed is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with plumed.  If not, see <http://www.gnu.org/licenses/>.
+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++ */
#include "MetricRegister.h"
#include "RMSDBase.h"
#include "tools/Matrix.h"
#include "tools/RMSD.h"

namespace PLMD{

class OptimalRMSD : public RMSDBase {
private:
  bool fast;
  RMSD myrmsd;
public:
  explicit OptimalRMSD(const ReferenceConfigurationOptions& ro);
  void read( const PDB& );
  double calc( const std::vector<Vector>& pos, ReferenceValuePack& myder, const bool& squared ) const ;
  bool pcaIsEnabledForThisReference(){ return true; }
  void setupRMSDObject(){ myrmsd.set(getAlign(),getDisplace(),getReferencePositions(),"OPTIMAL"); }
  void setupPCAStorage( ReferenceValuePack& mypack ){ 
        mypack.switchOnPCAOption();
        mypack.centeredpos.resize( getNumberOfAtoms() ); 
        mypack.displacement.resize( getNumberOfAtoms() ); 
        mypack.DRotDPos.resize(3,3); mypack.rot.resize(1);
  } 
  double projectAtomicDisplacementOnVector( const unsigned& iv, const Matrix<Vector>& vecs, const std::vector<Vector>& pos, ReferenceValuePack& mypack ) const ; 
};

PLUMED_REGISTER_METRIC(OptimalRMSD,"OPTIMAL")

OptimalRMSD::OptimalRMSD(const ReferenceConfigurationOptions& ro ):
ReferenceConfiguration(ro),
RMSDBase(ro)
{
  fast=ro.usingFastOption();
}

void OptimalRMSD::read( const PDB& pdb ){
  readReference( pdb ); myrmsd.set(getAlign(),getDisplace(),getReferencePositions(),"OPTIMAL"); 
}

double OptimalRMSD::calc( const std::vector<Vector>& pos, ReferenceValuePack& myder, const bool& squared ) const {
  double d; 
  if( myder.calcUsingPCAOption() ){
     std::vector<Vector> centeredreference( getNumberOfAtoms () );
     d=myrmsd.calc_PCAelements(pos,myder.getAtomVector(),myder.rot[0],myder.DRotDPos,myder.getAtomsDisplacementVector(),myder.centeredpos,centeredreference,squared);
     unsigned nat = pos.size(); for(unsigned i=0;i<nat;++i) myder.getAtomsDisplacementVector()[i] -= getReferencePosition(i);
  } else if( fast ){
     if( getAlign()==getDisplace() ) d=myrmsd.optimalAlignment<false,true>(getAlign(),getDisplace(),pos,getReferencePositions(),myder.getAtomVector(),squared); 
     d=myrmsd.optimalAlignment<false,false>(getAlign(),getDisplace(),pos,getReferencePositions(),myder.getAtomVector(),squared);
  } else {
     if( getAlign()==getDisplace() ) d=myrmsd.optimalAlignment<true,true>(getAlign(),getDisplace(),pos,getReferencePositions(),myder.getAtomVector(),squared);
     else d=myrmsd.optimalAlignment<true,false>(getAlign(),getDisplace(),pos,getReferencePositions(),myder.getAtomVector(),squared);
  }
  myder.clear(); for(unsigned i=0;i<pos.size();++i) myder.setAtomDerivatives( i, myder.getAtomVector()[i] ); 
  if( !myder.updateComplete() ) myder.updateDynamicLists();
  return d;
}

double OptimalRMSD::projectAtomicDisplacementOnVector( const unsigned& iv, const Matrix<Vector>& vecs, const std::vector<Vector>& pos, ReferenceValuePack& mypack ) const {
  plumed_dbg_assert( mypack.calcUsingPCAOption() );

  double proj=0.0; mypack.clear();
  for(unsigned i=0;i<pos.size();++i){
      proj += dotProduct( mypack.getAtomsDisplacementVector()[i] , vecs(iv,i) );
  }
  for(unsigned a=0;a<3;a++){
      for(unsigned b=0;b<3;b++){ 
          for(unsigned iat=0;iat<getNumberOfAtoms();iat++){
              double tmp1=0.;
              for(unsigned n=0;n<getNumberOfAtoms();n++) tmp1+=mypack.centeredpos[n][b]*vecs(iv,n)[a];
              mypack.addAtomDerivatives( iat, mypack.DRotDPos[a][b][iat]*tmp1 );
          }
      }
  }
  Tensor trot=mypack.rot[0].transpose();
  Vector v1; v1.zero(); double prefactor = 1. / static_cast<double>( getNumberOfAtoms() );
  for(unsigned n=0;n<getNumberOfAtoms();n++) v1+=prefactor*matmul(trot,vecs(iv,n));
  for(unsigned iat=0;iat<getNumberOfAtoms();iat++) mypack.addAtomDerivatives( iat, matmul(trot,vecs(iv,iat))-v1 );
  if( !mypack.updateComplete() ) mypack.updateDynamicLists();

  return proj;
}

}
