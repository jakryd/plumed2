/* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
   Copyright (c) 2011-2017 The plumed team
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
#include "ActionWithValue.h"
#include "ActionWithArguments.h"
#include "ActionAtomistic.h"
#include "ActionWithVirtualAtom.h"
#include "PlumedMain.h"
#include "ActionSet.h"
#include "ActionRegister.h"
#include "tools/Stopwatch.h"
#include "tools/Exception.h"
#include "tools/OpenMP.h"

using namespace std;
namespace PLMD {

void ActionWithValue::registerKeywords(Keywords& keys) {
  keys.setComponentsIntroduction("By default the value of the calculated quantity can be referenced elsewhere in the "
                                 "input file by using the label of the action.  Alternatively this Action can be used "
                                 "to calculate the following quantities by employing the keywords listed "
                                 "below.  These quanties can be referenced elsewhere in the input by using this Action's "
                                 "label followed by a dot and the name of the quantity required from the list below.");
  keys.addFlag("NUMERICAL_DERIVATIVES", false, "calculate the derivatives for these quantities numerically");
  keys.addFlag("SERIAL",false,"do the calculation in serial.  Do not parallelize");
  keys.addFlag("TIMINGS",false,"output information on the timings of the various parts of the calculation");
}

void ActionWithValue::noAnalyticalDerivatives(Keywords& keys) {
  keys.remove("NUMERICAL_DERIVATIVES");
  keys.addFlag("NUMERICAL_DERIVATIVES",true,"analytical derivatives are not implemented for this keyword so numerical derivatives are always used");
}

void ActionWithValue::componentsAreNotOptional(Keywords& keys) {
  keys.setComponentsIntroduction("By default this Action calculates the following quantities. These quanties can "
                                 "be referenced elsewhere in the input by using this Action's label followed by a "
                                 "dot and the name of the quantity required from the list below.");
}

void ActionWithValue::useCustomisableComponents(Keywords& keys) {
  keys.setComponentsIntroduction("The names of the components in this action can be customized by the user in the "
                                 "actions input file.  However, in addition to these customizable components the "
                                 "following quantities will always be output");
}

ActionWithValue::ActionWithValue(const ActionOptions&ao):
  Action(ao),
  noderiv(true),
  numericalDerivatives(false),
  no_openmp(false),
  serial(false),
  timers(false),
  in_a_chain(false),
  nactive_tasks(0),
  action_to_do_before(NULL),
  action_to_do_after(NULL)
{
  if( keywords.exists("NUMERICAL_DERIVATIVES") ) parseFlag("NUMERICAL_DERIVATIVES",numericalDerivatives);
  if(numericalDerivatives) log.printf("  using numerical derivatives\n");
  if( keywords.exists("SERIAL") ) parseFlag("SERIAL",serial); 
  else serial=true;
  if( keywords.exists("TIMINGS") ){
      parseFlag("TIMINGS",timers);
      if( timers ){ stopwatch.start(); stopwatch.pause(); }
  }
}

ActionWithValue::~ActionWithValue() {
  stopwatch.start(); stopwatch.stop();
  if(timers) {
    stopwatch.start(); stopwatch.stop();
    log.printf("timings for action %s with label %s \n", getName().c_str(), getLabel().c_str() );
    log<<stopwatch;
  }
}

ActionWithValue* ActionWithValue::getActionThatCalculates() {
  // Return this if we have no dependencies
  if( !action_to_do_before ) return this; 
  // Recursively go through actiosn before
  return action_to_do_before->getActionThatCalculates();
}

void ActionWithValue::getAllActionLabelsInChain( std::vector<std::string>& mylabels ) const {
  bool found = false ;
  for(unsigned i=0;i<mylabels.size();++i){
      if( getLabel()==mylabels[i] ){ found=true; }
  }
  if( !found ) mylabels.push_back( getLabel() );
  if( action_to_do_after ) action_to_do_after->getAllActionLabelsInChain( mylabels );
}

bool ActionWithValue::addActionToChain( const std::vector<std::string>& alabels, ActionWithValue* act ){
  if( action_to_do_after ){ bool state=action_to_do_after->addActionToChain( alabels, act ); return state; }

  // Check action is not already in chain
  std::vector<std::string> mylabels; getActionThatCalculates()->getAllActionLabelsInChain( mylabels );
  for(unsigned i=0;i<mylabels.size();++i){
      if( act->getLabel()==mylabels[i] ) return true; 
  }

  // Check that everything that is required has been calculated
  for(unsigned i=0;i<alabels.size();++i){
      bool found=false;
      for(unsigned j=0;j<mylabels.size();++j){
          if( alabels[i]==mylabels[j] ){ found=true; break; }
      } 
      if( !found ) return false; 
  }
  action_to_do_after=act; act->addDependency( this ); act->action_to_do_before=this; 
  return true;
}

void ActionWithValue::clearInputForces() {
  for(unsigned i=0; i<values.size(); i++) values[i]->clearInputForce();
}

void ActionWithValue::clearDerivatives( const bool& force ) {
  // This ensures we do not clear derivatives calculated in a chain until the next time the first 
  // action in the chain is called
  if( !force && action_to_do_before ) return ;
  unsigned nt = OpenMP::getGoodNumThreads(values); 
  #pragma omp parallel num_threads(nt)
  {
    #pragma omp for
    for(unsigned i=0; i<values.size(); i++) values[i]->clearDerivatives();
  }
  if( action_to_do_after ) action_to_do_after->clearDerivatives( true ); 
}

// -- These are the routine for copying the value pointers to other classes -- //

bool ActionWithValue::exists( const std::string& name ) const {
  for(unsigned i=0; i<values.size(); ++i) {
    if (values[i]->name==name) return true;
  }
  return false;
}

Value* ActionWithValue::copyOutput( const std::string& name ) const {
  for(unsigned i=0; i<values.size(); ++i) {
    if (values[i]->name==name) return values[i].get();
  }
  plumed_merror("there is no pointer with name " + name);
  return NULL;
}

Value* ActionWithValue::copyOutput( const unsigned& n ) const {
  plumed_massert(n<values.size(),"you have requested a pointer that is out of bounds");
  return values[n].get();
}

// -- HERE WE HAVE THE STUFF FOR THE DEFAULT VALUE -- //

void ActionWithValue::addValue( const std::vector<unsigned>& shape ) {
  plumed_massert(values.empty(),"You have already added the default value for this action");
  values.emplace_back(new Value(this,getLabel(), false, shape ) );
}

void ActionWithValue::addValueWithDerivatives( const std::vector<unsigned>& shape ) {
  if( shape.size()>0 && shape.size()!=getNumberOfDerivatives() ) plumed_merror("should not be adding non zero rank values with derivatives");
  plumed_massert(values.empty(),"You have already added the default value for this action");
  values.emplace_back(new Value(this,getLabel(), true, shape ) );
}

void ActionWithValue::setNotPeriodic() {
  plumed_massert(values.size()==1,"The number of components is not equal to one");
  plumed_massert(values[0]->name==getLabel(), "The value you are trying to set is not the default");
  values[0]->min=0; values[0]->max=0;
  values[0]->setupPeriodicity();
}

void ActionWithValue::setPeriodic( const std::string& min, const std::string& max ) {
  plumed_massert(values.size()==1,"The number of components is not equal to one");
  plumed_massert(values[0]->name==getLabel(), "The value you are trying to set is not the default");
  values[0]->setDomain( min, max );
}

Value* ActionWithValue::getPntrToValue() {
  plumed_dbg_massert(values.size()==1,"The number of components is not equal to one");
  plumed_dbg_massert(values[0]->name==getLabel(), "The value you are trying to retrieve is not the default");
  return values[0].get();
}

// -- HERE WE HAVE THE STUFF FOR NAMED VALUES / COMPONENTS -- //

void ActionWithValue::addComponent( const std::string& name, const std::vector<unsigned>& shape ) {
  if( !keywords.outputComponentExists(name,true) ) {
    warning("a description of component " + name + " has not been added to the manual. Components should be registered like keywords in "
            "registerKeywords as described in the developer docs.");
  }
  std::string thename; thename=getLabel() + "." + name;
  for(unsigned i=0; i<values.size(); ++i) {
    plumed_massert(values[i]->name!=getLabel(),"Cannot mix single values with components");
    plumed_massert(values[i]->name!=thename&&name!="bias","Since PLUMED 2.3 the component 'bias' is automatically added to all biases by the general constructor!\n"
                   "Remove the line addComponent(\"bias\") from your bias.");
    plumed_massert(values[i]->name!=thename,"there is already a value with this name");
  }
  values.emplace_back(new Value(this,thename, false, shape ) );
  std::string msg="  added component to this action:  "+thename+" \n";
  log.printf(msg.c_str());
}

void ActionWithValue::addComponentWithDerivatives( const std::string& name, const std::vector<unsigned>& shape ) {
  if( !keywords.outputComponentExists(name,true) ) {
    warning("a description of component " + name + " has not been added to the manual. Components should be registered like keywords in "
            "registerKeywords as described in the developer doc.");
  }
  std::string thename; thename=getLabel() + "." + name;
  for(unsigned i=0; i<values.size(); ++i) {
    plumed_massert(values[i]->name!=getLabel(),"Cannot mix single values with components");
    plumed_massert(values[i]->name!=thename&&name!="bias","Since PLUMED 2.3 the component 'bias' is automatically added to all biases by the general constructor!\n"
                   "Remove the line addComponentWithDerivatives(\"bias\") from your bias.");
    plumed_massert(values[i]->name!=thename,"there is already a value with this name");
  }
  values.emplace_back(new Value(this,thename, true, shape ) );
  std::string msg="  added component to this action:  "+thename+" \n";
  log.printf(msg.c_str());
}

int ActionWithValue::getComponent( const std::string& name ) const {
  plumed_massert( !exists( getLabel() ), "You should not be calling this routine if you are using a value");
  std::string thename; thename=getLabel() + "." + name;
  for(unsigned i=0; i<values.size(); ++i) {
    if (values[i]->name==thename) return i;
  }
  plumed_merror("there is no component with name " + name);
  return -1;
}

std::string ActionWithValue::getComponentsList( ) const {
  std::string complist;
  for(unsigned i=0; i<values.size(); ++i) {
    complist+=values[i]->name+" ";
  }
  return complist;
}

std::vector<std::string> ActionWithValue::getComponentsVector( ) const {
  std::vector<std::string> complist;
  for(unsigned i=0; i<values.size(); ++i) {
    complist.push_back(values[i]->name);
  }
  return complist;
}

void ActionWithValue::componentIsNotPeriodic( const std::string& name ) {
  int kk=getComponent(name);
  values[kk]->min=0; values[kk]->max=0;
  values[kk]->setupPeriodicity();
}

void ActionWithValue::componentIsPeriodic( const std::string& name, const std::string& min, const std::string& max ) {
  int kk=getComponent(name);
  values[kk]->setDomain(min,max);
}

void ActionWithValue::setGradientsIfNeeded() {
  if(isOptionOn("GRADIENTS")) {
    for(unsigned i=0; i<values.size(); i++) values[i]->setGradients();
  }
}

void ActionWithValue::turnOnDerivatives() {
  // Turn on the derivatives in all actions on which we are dependent
  for(const auto & p : getDependencies() ) {
    ActionWithValue* vv=dynamic_cast<ActionWithValue*>(p);
    if(vv) vv->turnOnDerivatives();
  }
  // Turn on the derivatives
  noderiv=false;
  // Resize the derivatives
  for(unsigned i=0; i<values.size(); ++i) values[i]->resizeDerivatives( getNumberOfDerivatives() );
}

Value* ActionWithValue::getPntrToOutput( const unsigned& ind ) const {
  plumed_dbg_massert(ind<values.size(),"you have requested a pointer that is out of bounds");
  return values[ind];
}

Value* ActionWithValue::getPntrToComponent( const std::string& name ){
  int kk=getComponent(name);
  return values[kk].get();
}

Value* ActionWithValue::getPntrToComponent( int n ) {
  plumed_dbg_massert(n<values.size(),"you have requested a pointer that is out of bounds");
  return values[n].get();
}

void ActionWithValue::interpretDataLabel( const std::string& mystr, Action* myuser, std::vector<Value*>& args ){
  // Check for streams
  unsigned nstr=0; for(unsigned i=0;i<values.size();++i){ if( values[i]->getRank()>0 ) nstr++; }

  if( mystr=="" || mystr==getLabel() ){
      // Retrieve value with name of action
      if( values[0]->name!=getLabel() ) myuser->error("action " + getLabel() + " does not have a value");
      args.push_back( values[0] ); values[0]->interpretDataRequest( myuser->getLabel(), "" ); 
  } else if( mystr==getLabel() + ".*" ){
      // Retrieve all scalar values
      if( !action_to_do_after ){
          retrieveAllScalarValuesInLoop( args ); 
      } else if( actionRegister().checkForShortcut(getName()) ) {  
          Keywords skeys; actionRegister().getShortcutKeywords( getName(), skeys );
          std::vector<std::string> out_comps( skeys.getAllOutputComponents() );
          for(unsigned i=0;i<out_comps.size();++i){
              std::string keyname; bool donumtest = skeys.getKeywordForThisOutput( out_comps[i], keyname );
              if( donumtest ) {
                  if( skeys.numbered( keyname ) ){
                      for(unsigned j=1;;++j){
                          std::string numstr; Tools::convert( j, numstr );
                          ActionWithValue* action=plumed.getActionSet().selectWithLabel<ActionWithValue*>( getLabel() + out_comps[i] + numstr );
                          if( !action ) break;
                          args.push_back( action->getPntrToValue() ); 
                      }
                  }
              } 
              ActionWithValue* action=plumed.getActionSet().selectWithLabel<ActionWithValue*>( getLabel() + out_comps[i] );
              if( action ) args.push_back( action->getPntrToValue() );
          }
          if( args.size()==0 ) myuser->error("could not find any actions created by shortcuts in action");
      }
      for(unsigned j=0;j<args.size();++j) args[j]->interpretDataRequest( myuser->getLabel(), "" );
  } else if( mystr.find(".")!=std::string::npos && exists( mystr ) ) {
      // Retrieve value with specific name
      args.push_back( copyOutput( mystr ) ); copyOutput( mystr )->interpretDataRequest( myuser->getLabel(), "" ); 
  } else {
      std::size_t dot1 = mystr.find_first_of('.'); std::string thelab = mystr.substr(0,dot1); 
      plumed_assert( thelab==getLabel() ); std::string rest = mystr.substr(dot1+1); 
      if( rest.find_first_of('.')==std::string::npos ){
         plumed_assert( values.size()==1 ); plumed_assert( values[0]->getRank()>0 && values[0]->getName()==getLabel() );
         args.push_back( values[0] ); values[0]->interpretDataRequest( myuser->getLabel(), rest ); 
      } else {
         std::size_t dot2 = rest.find_first_of('.'); std::string thecomp = rest.substr(0,dot2);
         if( !exists( thelab + "." + thecomp ) ) myuser->error("could not find component with label " + thelab + "." + thecomp );
         args.push_back( copyOutput( thelab + "." + thecomp ) ); 
         getPntrToComponent( thecomp )->interpretDataRequest( myuser->getLabel(), rest.substr(dot2+1) );
      }
  }
}

void ActionWithValue::addTaskToList( const unsigned& taskCode ) {
  fullTaskList.push_back( taskCode ); taskFlags.push_back(0);
  plumed_assert( fullTaskList.size()==taskFlags.size() );
}

void ActionWithValue::selectActiveTasks( std::vector<unsigned>& tflags ){
  buildCurrentTaskList( tflags );
  if( action_to_do_after ) action_to_do_after->selectActiveTasks( tflags );
//  for(unsigned i=0;i<values.size();++i){
//      if( values[i]->getRank()>0 ) values[i]->activateTasks( tflags );
//  }
}

void ActionWithValue::runAllTasks() {
// Skip this if this is done elsewhere 
  if( action_to_do_before ) return;
  
  unsigned stride=comm.Get_size();
  unsigned rank=comm.Get_rank();
  if(serial) { stride=1; rank=0; }

  // Build the list of active tasks 
  taskFlags.assign(taskFlags.size(),0); selectActiveTasks( taskFlags ); nactive_tasks = 0;
  for(unsigned i=0; i<fullTaskList.size(); ++i) {
    if( taskFlags[i]>0 ) nactive_tasks++;
  }

  // Get number of threads for OpenMP
  unsigned nt=OpenMP::getNumThreads();
  if( nt*stride*10>nactive_tasks ) nt=nactive_tasks/stride/10;
  if( nt==0 || no_openmp ) nt=1;

  // Now create the partial task list
  unsigned n=0; partialTaskList.resize( nactive_tasks ); indexOfTaskInFullList.resize( nactive_tasks );
  for(unsigned i=0; i<fullTaskList.size(); ++i) {
    // Deactivate sets inactive tasks to number not equal to zero
    if( taskFlags[i]>0 ) {
      partialTaskList[n] = fullTaskList[i]; indexOfTaskInFullList[n]=i; n++;
    }
  }

  // Get the total number of streamed quantities that we need
  unsigned nquantities = 0, ncols=0, nmatrices=0; 
  getNumberOfStreamedQuantities( nquantities, ncols, nmatrices );
  setupVirtualAtomStashes( nquantities );
  // Get size for buffer
  unsigned bufsize=0; getSizeOfBuffer( nactive_tasks, bufsize );
  if( buffer.size()!=bufsize ) buffer.resize( bufsize );
  // Clear buffer
  buffer.assign( buffer.size(), 0.0 );

  // Recover the number of derivatives we require
  unsigned nderivatives = 0;
  if( !noderiv ) getNumberOfStreamedDerivatives( nderivatives );
  // Perform all tasks required before main loop
  prepareForTasks();

  if(timers) stopwatch.start("2 Loop over tasks");
  #pragma omp parallel num_threads(nt)
  {
    std::vector<double> omp_buffer;
    if( nt>1 ) omp_buffer.resize( bufsize, 0.0 );
    MultiValue myvals( nquantities, nderivatives, ncols, nmatrices );
    myvals.clearAll();

    #pragma omp for nowait
    for(unsigned i=rank; i<nactive_tasks; i+=stride) {
      // Calculate the stuff in the loop for this action
      runTask( indexOfTaskInFullList[i], partialTaskList[i], myvals );

      // Now transfer the data to the actions that accumulate values from the calculated quantities
      if( nt>1 ) {
        gatherAccumulators( indexOfTaskInFullList[i], myvals, omp_buffer );
      } else {
        gatherAccumulators( indexOfTaskInFullList[i], myvals, buffer );
      }

      // Clear the value
      myvals.clearAll();
    }
    #pragma omp critical
    if(nt>1) for(unsigned i=0; i<bufsize; ++i) buffer[i]+=omp_buffer[i];
  }
  if(timers) stopwatch.stop("2 Loop over tasks");

  if(timers) stopwatch.start("3 MPI gather");
  // MPI Gather everything
  if( !serial && buffer.size()>0 ) comm.Sum( buffer );
  // Update the elements that are makign contributions to the sum here
  // this causes problems if we do it in prepare
  if(timers) stopwatch.stop("3 MPI gather");

  if(timers) stopwatch.start("4 Finishing computations");
  finishComputations( buffer );
  if(timers) stopwatch.stop("4 Finishing computations");
}

void ActionWithValue::getNumberOfStreamedDerivatives( unsigned& nderivatives ) const {
  unsigned nnd = getNumberOfDerivatives(); if( nnd>nderivatives ) nderivatives = nnd;
  if( action_to_do_after ) action_to_do_after->getNumberOfStreamedDerivatives( nderivatives );
}

void ActionWithValue::setupVirtualAtomStashes( unsigned& nquants ) {
  ActionWithVirtualAtom* av = dynamic_cast<ActionWithVirtualAtom*>( this );
  if( av ) av->setStashIndices( nquants );
  if( action_to_do_after ) action_to_do_after->setupVirtualAtomStashes( nquants );
}

void ActionWithValue::getNumberOfStreamedQuantities( unsigned& nquants, unsigned& ncols, unsigned& nmat ) const {
  for(unsigned i=0;i<values.size();++i){ 
     if( values[i]->getRank()==2 ){ 
         if( values[i]->getShape()[1]>ncols ){ ncols = values[i]->getShape()[1]; }
         values[i]->matpos=nmat; nmat++; 
     }
     values[i]->streampos=nquants; nquants++; 
  }
  if( action_to_do_after ) action_to_do_after->getNumberOfStreamedQuantities( nquants, ncols, nmat );
}

void ActionWithValue::getSizeOfBuffer( const unsigned& nactive_tasks, unsigned& bufsize ){
  for(unsigned i=0;i<values.size();++i){
      values[i]->bufstart=bufsize; 
      if( values[i]->getRank()==0 && values[i]->hasDerivatives() ) bufsize += 1 + values[i]->getNumberOfDerivatives();
      else if( values[i]->getRank()==0 ) bufsize += 1;
      else if( values[i]->storedata ){
          if( values[i]->hasDeriv ) bufsize += values[i]->getSize(); 
          else if( values[i]->getRank()==2 ) bufsize += nactive_tasks*values[i]->getShape()[1];
          else bufsize += nactive_tasks;
      }
      // if( values[i]->getRank()==0 ) bufsize += 1 + values[i]->getNumberOfDerivatives();
  }
  if( action_to_do_after ) action_to_do_after->getSizeOfBuffer( nactive_tasks, bufsize );
}

void ActionWithValue::prepareForTasks(){
  if( action_to_do_after ) action_to_do_after->prepareForTasks();
}

void ActionWithValue::runTask( const std::string& controller, const unsigned& task_index, const unsigned& current, const unsigned colno, MultiValue& myvals ) const {
  // Do matrix element task
  myvals.setTaskIndex(task_index); myvals.setSecondTaskIndex( colno ); performTask( controller, current, colno, myvals );
  const ActionWithArguments* aa = dynamic_cast<const ActionWithArguments*>( this );
  if( aa ){
      if( actionInChain() ) { 
          // Now check if the task takes a matrix as input - if it does do it
          bool do_this_task = ((aa->getPntrToArgument(0))->getRank()==2);
#ifdef DNDEBUG
          if( do_this_task ){
              for(unsigned i=1;i<aa->getNumberOfArguments();++i) plumed_dbg_assert( (aa->getPntrToArgument(i))->getRank()==2 ); 
          }
#endif
          if( do_this_task ){ myvals.vector_call=false; myvals.setTaskIndex(task_index); performTask( current, myvals ); }
      }
  }

  // Check if we need to store stuff
  bool matrix=true; 
  for(unsigned i=0;i<values.size();++i){
      if( values[i]->getRank()!=2 ){ matrix=false; break; }
  }
  if( matrix ){  
      unsigned col_stash_index = colno; if( colno>getFullNumberOfTasks() ) col_stash_index = colno - getFullNumberOfTasks();
      for(unsigned i=0;i<values.size();++i){
          if( values[i]->storedata ) myvals.stashMatrixElement( values[i]->getPositionInMatrixStash(), col_stash_index, myvals.get( values[i]->getPositionInStream() ) );
      }
  }
  
  // Now continue on with the stream
  if( action_to_do_after ){
      if( action_to_do_after->isActive() ) action_to_do_after->runTask( controller, task_index, current, colno, myvals );
  }
}

void ActionWithValue::runTask( const unsigned& task_index, const unsigned& current, MultiValue& myvals ) const {
  myvals.setTaskIndex(task_index); myvals.vector_call=true; performTask( current, myvals ); 
  if( action_to_do_after ){
     if( action_to_do_after->isActive() ) action_to_do_after->runTask( task_index, current, myvals );
  }
}

void ActionWithValue::clearMatrixElements( MultiValue& myvals ) const {
  for(unsigned i=0;i<values.size();++i){
      if( values[i]->getRank()==2 ) myvals.clear( values[i]->getPositionInStream() ); 
  }
  if( action_to_do_after ){
     if( action_to_do_after->isActive() ) action_to_do_after->clearMatrixElements( myvals );
  }
}

void ActionWithValue::rerunTask( const unsigned& task_index, MultiValue& myvals ) const {
  if( !action_to_do_before ) {
      unsigned ncol=0, nmat=0, nquantities = 0; getNumberOfStreamedQuantities( nquantities, ncol, nmat ); 
      unsigned nderivatives = 0; getNumberOfStreamedDerivatives( nderivatives );
      if( myvals.getNumberOfValues()!=nquantities || myvals.getNumberOfDerivatives()!=nderivatives ) myvals.resize( nquantities, nderivatives, ncol, nmat );
      runTask( task_index, fullTaskList[task_index], myvals ); return;
  }
  action_to_do_before->rerunTask( task_index, myvals );
  
}

void ActionWithValue::gatherAccumulators( const unsigned& taskCode, const MultiValue& myvals, std::vector<double>& buffer ) const {
  for(unsigned i=0;i<values.size();++i){
      unsigned sind = values[i]->streampos, bufstart = values[i]->bufstart; 
      if( values[i]->getRank()==0 ){
           plumed_dbg_massert( bufstart<buffer.size(), "problem in " + getLabel() );
           buffer[bufstart] += myvals.get(sind);
           if( values[i]->hasDerivatives() ){
               unsigned ndmax = (values[i]->getPntrToAction())->getNumberOfDerivatives();
               for(unsigned k=0;k<myvals.getNumberActive(sind);++k){
                   unsigned kindex = myvals.getActiveIndex(sind,k);
                   plumed_dbg_massert( bufstart+1+kindex<buffer.size(), "problem in " + getLabel()  );
                   buffer[bufstart + 1 + kindex] += myvals.getDerivative(sind,kindex);
               }
           }
      } else if( values[i]->storedata ){
           // This looks after storing for matrices 
           if( values[i]->getRank()==2 && !values[i]->hasDeriv ){
              unsigned ncols = values[i]->getShape()[1];
              unsigned vindex = bufstart + taskCode*ncols; unsigned matind = values[i]->getPositionInMatrixStash();
              for(unsigned j=0;j<myvals.getNumberOfStashedMatrixElements(matind);++j){
                  unsigned jind = myvals.getStashedMatrixIndex(matind,j);
                  plumed_dbg_massert( vindex+jind<buffer.size(), "failing in " + getLabel() );
                  buffer[vindex + jind] += myvals.getStashedMatrixElement( matind, jind );
              }
           // This looks after storing in all other cases 
           } else {
              unsigned nspace=1; if( values[i]->hasDeriv ) nspace=(1 + values[i]->getNumberOfDerivatives() );
              unsigned vindex = bufstart + taskCode*nspace; plumed_dbg_massert( vindex<buffer.size(), "failing in " + getLabel() ); 
              buffer[vindex] += myvals.get(sind);
           }
      } 
  }

  // Special method for dealing with centers
  const ActionWithVirtualAtom* av = dynamic_cast<const ActionWithVirtualAtom*>( this );
  if( av ) av->gatherForVirtualAtom( myvals, buffer );

  if( action_to_do_after ){
     if( action_to_do_after->isActive() ) action_to_do_after->gatherAccumulators( taskCode, myvals, buffer );
  }
}

void ActionWithValue::retrieveAllScalarValuesInLoop( std::vector<Value*>& myvals ){
  for(unsigned i=0;i<values.size();++i){
      if( values[i]->getRank()==0 ){
          bool found=false;
          for(unsigned j=0;j<myvals.size();++j){
              if( values[i]->getName()==myvals[j]->getName() ){ found=true; break; }
          }
          if( !found ) myvals.push_back( values[i] );
      }
  }
  if( action_to_do_after ) action_to_do_after->retrieveAllScalarValuesInLoop( myvals );
}

void ActionWithValue::finishComputations( const std::vector<double>& buffer ){
  for(unsigned i=0;i<values.size();++i){
      unsigned bufstart = values[i]->bufstart; 
      if( values[i]->reset ) values[i]->data.assign( values[i]->data.size(), 0 );
      if( values[i]->storedata ){
          for(unsigned j=0;j<values[i]->getSize();++j) values[i]->add( j, buffer[bufstart+j] ); 
      }
      if( !doNotCalculateDerivatives() && values[i]->hasDeriv && values[i]->getRank()==0 ){ 
          for(unsigned j=0;j<values[i]->getNumberOfDerivatives();++j) values[i]->setDerivative( j, buffer[bufstart+1+j] );
      }
  }
  transformFinalValueAndDerivatives( buffer );
  if( action_to_do_after ){
     if( action_to_do_after->isActive() ) action_to_do_after->finishComputations( buffer );
  }
}

bool ActionWithValue::getForcesFromValues( std::vector<double>& forces ) {
   unsigned type=0;
   if( values[0]->shape.size()==0 && values[0]->hasDeriv ) type=1;
   else if( values[0]->hasDeriv ) type=2;
   else plumed_assert( values[0]->shape.size()>0 );

#ifdef DNDEBUG
   if( type==0 ) {
       for(unsigned i=0;i<values.size();++i) plumed_dbg_assert( values[i]->shape.size()>0 && !values[i]->hasDeriv );
   } else if( type==1 ) {
       for(unsigned i=0;i<values.size();++i) plumed_dbg_assert( values[i]->shape.size()==0 ); 
   } else if( type==2 ) {
       for(unsigned i=0;i<values.size();++i) plumed_dbg_assert( values[i]->shape.size()>0 && values[i]->hasDeriv );
   } else {
       plumed_merror("value type not defined");
   }
#endif
   bool at_least_one_forced=false;
   if( type==1 || type==2 ) { 
       for(unsigned i=0;i<values.size();++i){
           if( values[i]->applyForce( forces ) ) at_least_one_forced=true;
       }
   } else {
      // Check if there are any forces
      for(unsigned i=0;i<values.size();++i){
          if( values[i]->hasForce ) at_least_one_forced=true;
      }
      if( !at_least_one_forced ) return false;

      // Get the action that calculates these quantitites
      ActionWithValue* av = getActionThatCalculates();
      nactive_tasks = av->nactive_tasks;
      // Setup MPI parallel loop
      unsigned stride=comm.Get_size();
      unsigned rank=comm.Get_rank();
      if(serial) { stride=1; rank=0; }

      // Get number of threads for OpenMP
      unsigned nt=OpenMP::getNumThreads();
      if( nt*stride*10>nactive_tasks ) nt=nactive_tasks/stride/10;
      if( nt==0 || no_openmp ) nt=1;

      // Now determine how big the multivalue needs to be
      unsigned nderiv=0; av->getNumberOfStreamedDerivatives( nderiv );
      unsigned nquants=0, ncols=0, nmatrices=0; av->getNumberOfStreamedQuantities( nquants, ncols, nmatrices );
      #pragma omp parallel num_threads(nt)
      {
        std::vector<double> omp_forces;
        if( nt>1 ) omp_forces.resize( forces.size(), 0.0 );
        MultiValue myvals( nquants, nderiv, ncols, nmatrices );
        myvals.clearAll();

        #pragma omp for nowait
        for(unsigned i=rank;i<nactive_tasks;i+=stride){
            unsigned itask = av->indexOfTaskInFullList[i]; 
            av->runTask( itask, av->partialTaskList[i], myvals );
            for(unsigned k=0;k<values.size();++k){
                unsigned sspos = values[k]->streampos; double fforce = values[k]->getForce(itask);
                if( nt>1 ) {
                   for(unsigned j=0;j<myvals.getNumberActive(sspos);++j){
                        unsigned jder=myvals.getActiveIndex(sspos, j);
                        omp_forces[jder] += fforce*myvals.getDerivative( sspos, jder );
                    }
                } else {
                    for(unsigned j=0;j<myvals.getNumberActive(sspos);++j){
                        unsigned jder=myvals.getActiveIndex(sspos, j);
                        forces[jder] += fforce*myvals.getDerivative( sspos, jder );
                    }
                }
            }
            myvals.clearAll();
        }
        #pragma omp critical
        if(nt>1) for(unsigned i=0; i<forces.size(); ++i) forces[i]+=omp_forces[i]; 
      }
      // MPI Gather on forces
      if( !serial ) comm.Sum( forces );
   }
   return at_least_one_forced;
}

}
