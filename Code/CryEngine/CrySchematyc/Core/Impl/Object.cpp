// Copyright 2001-2016 Crytek GmbH / Crytek Group. All rights reserved.

#include "StdAfx.h"
#include "Object.h"

#include <CrySchematyc/Action.h>

#include <CrySchematyc/IObjectProperties.h>
#include <CrySchematyc/Env/IEnvRegistry.h>
#include <CrySchematyc/Env/Elements/IEnvAction.h>
#include <CrySchematyc/Env/Elements/IEnvComponent.h>
#include <CrySchematyc/Reflection/ActionDesc.h>
#include <CrySchematyc/Runtime/IRuntimeClass.h>
#include <CrySchematyc/Runtime/RuntimeParamMap.h>
#include <CrySchematyc/Runtime/RuntimeParams.h>
#include <CrySchematyc/Services/IUpdateScheduler.h>
#include <CrySchematyc/Utils/Assert.h>
#include <CrySchematyc/Utils/STLUtils.h>

#include "Core.h"
#include "CVars.h"
#include "CoreEnv/CoreEnvSignals.h"
#include "Runtime/RuntimeRegistry.h"

#include <CryEntitySystem/IEntitySystem.h>
#include <CryEntitySystem/IEntity.h>
#include <CryEntitySystem/IEntityComponent.h>

namespace Schematyc
{

CObject::SStateMachine::SStateMachine()
	: stateIdx(InvalidIdx)
{}

CObject::SAction::SAction(const CActionDesc& _desc, const SRuntimeActionDesc& _runtimeDesc, const CActionPtr& _ptr)
	: desc(_desc)
	, runtimeDesc(_runtimeDesc)
	, ptr(_ptr)
{}

CObject::STimer::STimer(CObject* _pObject, const CryGUID& _guid, TimerFlags _flags)
	: pObject(_pObject)
	, guid(_guid)
	, flags(_flags)
{}

void CObject::STimer::Activate()
{
	SCHEMATYC_CORE_ASSERT(pObject);
	if (pObject)
	{
		pObject->ProcessSignal(SObjectSignal(guid));
	}
}

CObject::CObject(ObjectId id)
	: m_id(id)
	, m_simulationMode(ESimulationMode::Idle)
	, m_bQueueSignals(false)
{}

CObject::~CObject()
{
	Stop(true);
	DestroyTimers();
	DestroyActions();
	DestroyComponents();
	DestroyStateMachines();

	if (m_pEntity != nullptr)
	{
		m_pEntity->OnSchematycObjectDestroyed();
	}
}

bool CObject::Init(const CRuntimeClassConstPtr& pClass, void* pCustomData, const IObjectPropertiesPtr& pProperties, ESimulationMode simulationMode, IEntity* pEntity)
{
	m_pEntity = pEntity;

	if (!SetClass(pClass))
	{
		return false;
	}

	m_pCustomData = pCustomData;
	m_pProperties = pProperties;

	if (!Start(simulationMode, true))
	{
		return false;
	}

	return true;
}

ObjectId CObject::GetId() const
{
	return m_id;
}

const IRuntimeClass& CObject::GetClass() const
{
	SCHEMATYC_CORE_ASSERT(m_pClass);
	return *m_pClass;
}

void* CObject::GetCustomData() const
{
	return m_pCustomData;
}

ESimulationMode CObject::GetSimulationMode() const
{
	return m_simulationMode;
}

bool CObject::Reset(ESimulationMode simulationMode, EObjectResetPolicy resetPolicy)
{
	if ((simulationMode != m_simulationMode) || (resetPolicy == EObjectResetPolicy::Always))
	{
		//if (m_simulationMode != ESimulationMode::Idle)
		{
			Stop(false);
		}

		if (simulationMode != ESimulationMode::Idle)
		{
			CRuntimeClassConstPtr pClass = CCore::GetInstance().GetRuntimeRegistryImpl().GetClassImpl(m_pClass->GetGUID());
			if (pClass->GetTimeStamp() > m_pClass->GetTimeStamp())
			{
				SetClass(pClass);
			}

			if (!Start(simulationMode, true))
			{
				Stop(false);
				return false;
			}
		}
	}
	return true;
}

void CObject::ProcessSignal(const SObjectSignal& signal)
{
	if (m_bQueueSignals)
	{
		m_signalQueue.emplace_back(signal);
	}
	else
	{
		m_bQueueSignals = true;

		ExecuteSignalReceivers(signal);

		const uint32 stateMachineCount = m_stateMachines.size();
		for (uint32 stateMachineIdx = 0; stateMachineIdx < stateMachineCount; ++stateMachineIdx)
		{
			ExecuteStateSignalReceivers(stateMachineIdx, signal);
		}

		for (uint32 stateMachineIdx = 0; stateMachineIdx < stateMachineCount; ++stateMachineIdx)
		{
			if (EvaluateStateTransitions(stateMachineIdx, signal))
			{
				break;
			}
		}

		SCHEMATYC_CORE_ASSERT(m_bQueueSignals);
		m_bQueueSignals = false;
		ProcessSignalQueue();
	}
}

void CObject::StopAction(CAction& action)
{
	for (SAction& _action : m_actions)
	{
		if (_action.ptr.get() == &action)
		{
			if (_action.bRunning)
			{
				action.Stop();
				_action.bRunning = false;
			}
		}
	}
}

EVisitResult CObject::VisitComponents(const ObjectComponentConstVisitor& visitor) const
{
	SCHEMATYC_CORE_ASSERT(visitor);
	if (visitor)
	{
		for (const SComponent& component : m_components)
		{
			if (!component.pComponent)
				continue;
			const EVisitStatus status = visitor(*component.pComponent);
			switch (status)
			{
			case EVisitStatus::Stop:
				{
					return EVisitResult::Stopped;
				}
			case EVisitStatus::Error:
				{
					return EVisitResult::Error;
				}
			}
		}
	}
	return EVisitResult::Complete;
}

void CObject::Dump(IObjectDump& dump, const ObjectDumpFlags& flags) const
{
	if (flags.Check(EObjectDumpFlags::States))
	{
		const RuntimeClassStateMachines& classStateMachines = m_pClass->GetStateMachines();
		const RuntimeClassStates& classStates = m_pClass->GetStates();
		for (uint32 stateMachineIdx = 0, stateMachineCount = m_stateMachines.size(); stateMachineIdx < stateMachineCount; ++stateMachineIdx)
		{
			const SRuntimeClassStateMachine& classStateMachine = classStateMachines[stateMachineIdx];
			IObjectDump::SStateMachine dumpStateMachine(classStateMachine.guid, classStateMachine.name.c_str());

			const uint32 stateIdx = m_stateMachines[stateMachineIdx].stateIdx;
			if (stateIdx != InvalidIdx)
			{
				const SRuntimeClassState& classState = classStates[stateIdx];
				dumpStateMachine.state.guid = classState.guid;
				dumpStateMachine.state.szName = classState.name.c_str();
			}
			dump(dumpStateMachine);
		}
	}

	if (flags.Check(EObjectDumpFlags::Variables))
	{
		const RuntimeClassVariables& classVariables = m_pClass->GetVariables();
		for (const SRuntimeClassVariable& classVariable : classVariables)
		{
			const IObjectDump::SVariable dumpVariable(classVariable.guid, classVariable.name.c_str(), *m_scratchpad.Get(classVariable.pos));
			dump(dumpVariable);
		}
	}

	if (flags.Check(EObjectDumpFlags::Timers))
	{
		ITimerSystem& timerSystem = gEnv->pSchematyc->GetTimerSystem();
		const RuntimeClassTimers& classTimers = m_pClass->GetTimers();
		for (uint32 timerIdx = 0, timerCount = m_timers.size(); timerIdx < timerCount; ++timerIdx)
		{
			const SRuntimeClassTimer& classTimer = classTimers[timerIdx];
			IObjectDump::STimer dumpTimer(classTimer.guid, classTimer.name.c_str(), timerSystem.GetTimeRemaining(m_timers[timerIdx].id));
			dump(dumpTimer);
		}
	}
}

CScratchpad& CObject::GetScratchpad()
{
	return m_scratchpad;
}

bool CObject::ExecuteFunction(uint32 functionIdx, CRuntimeParamMap& params)
{
	const RuntimeClassFunctions& classFunctions = m_pClass->GetFunctions();
	if (functionIdx < classFunctions.size())
	{
		const bool bPrevQueueSignals = m_bQueueSignals;
		m_bQueueSignals = true;

		const SRuntimeClassFunction& classFunction = classFunctions[functionIdx];
		ExecuteFunction(classFunction.graphIdx, params, classFunction.activationParams);

		if (!bPrevQueueSignals)
		{
			ProcessSignalQueue();
		}
		m_bQueueSignals = bPrevQueueSignals;

		return true;
	}
	return false;
}

bool CObject::StartAction(uint32 actionIdx, CRuntimeParamMap& params)
{
	if (actionIdx < m_actions.size())
	{
		SAction& action = m_actions[actionIdx];
		if (!action.bRunning)
		{
			RuntimeParamMap::ToInputClass(action.desc, action.ptr.get(), params);
			action.ptr->Start();
			action.bRunning = true;
			return true;
		}
	}
	return false;
}

IEntityComponent* CObject::GetComponent(uint32 componentIdx)
{
	return componentIdx < m_components.size() ? m_components[componentIdx].pComponent.get() : nullptr;
}

bool CObject::SetClass(const CRuntimeClassConstPtr& pClass)
{
	SCHEMATYC_CORE_ASSERT(pClass)
	if (!pClass)
	{
		return false;
	}

	DestroyTimers();
	DestroyActions();
	DestroyComponents();
	DestroyStateMachines();

	m_graphs.clear();
	m_stateMachines.clear();

	m_actions.clear();
	m_timers.clear();

	m_pClass = pClass;
	m_scratchpad = pClass->GetScratchpad(); // #SchematycTODO : Do we still need to do this here? We also reset the scratchpad in the start function.

	CreateGraphs();

	if (!CreateStateMachines())
	{
		return false;
	}

	if (!CreateComponents())
	{
		return false;
	}

	if (!CreateActions())
	{
		return false;
	}

	if (!CreateTimers())
	{
		return false;
	}

	RegisterForUpdate();

	return true;
}

bool CObject::Start(ESimulationMode simulationMode, bool bInitComponents)
{
	m_scratchpad = m_pClass->GetScratchpad();

	ResetGraphs();

	if (!ReadProperties())
	{
		return false;
	}

	if (bInitComponents)
	{
		if (!InitComponents())
		{
			return false;
		}
	}

	if (!InitActions())
	{
		return false;
	}

	m_simulationMode = simulationMode;

	ExecuteConstructors(simulationMode);

	if (bInitComponents)
	{
		RunComponents(simulationMode);
	}

	switch (simulationMode)
	{
	case ESimulationMode::Game:
		{
			ExecuteSignalReceivers(SObjectSignal::FromSignalClass(SStartSignal()));

			StartStateMachines(simulationMode);
			StartTimers(simulationMode);
			break;
		}
	}

	return true;
}

void CObject::Stop(bool bShutDownComponents)
{
	StopTimers();
	StopStateMachines();

	switch (m_simulationMode)
	{
	case ESimulationMode::Game:
		{
			ExecuteSignalReceivers(SObjectSignal::FromSignalClass(SStopSignal()));
			break;
		}
	}

	ShutdownActions();
	if (bShutDownComponents)
	{
		ShutdownComponents();
	}

	m_simulationMode = ESimulationMode::Idle;
}

void CObject::Update(const SUpdateContext& updateContext)
{
	// TODO: Update should actually never get called for preview objects when we are in game mode.
	const bool isInGame = gEnv->IsEditorGameMode();
	if (!isInGame || (isInGame && m_simulationMode != ESimulationMode::Preview))
	{
		ProcessSignal(SObjectSignal::FromSignalClass(SUpdateSignal(updateContext.frameTime)));
	}
	// ~TODO
}

void CObject::CreateGraphs()
{
	const uint32 graphCount = m_pClass->GetGraphCount();
	m_graphs.reserve(graphCount);
	for (uint32 graphIdx = 0; graphIdx < graphCount; ++graphIdx)
	{
		m_graphs.push_back(CRuntimeGraphInstance(*m_pClass->GetGraph(graphIdx)));
	}
}

void CObject::ResetGraphs()
{
	for (CRuntimeGraphInstance& graph : m_graphs)
	{
		graph.Reset();
	}
}

bool CObject::ReadProperties()
{
	const RuntimeClassComponentInstances& classComponentInstances = m_pClass->GetComponentInstances();
	for (uint32 componentIdx = 0, componentCount = m_components.size(); componentIdx < componentCount; ++componentIdx)
	{
		SComponent& component = m_components[componentIdx];

		bool bPublicPropertiesApplied = false;
		if (m_pProperties && component.classComponentInstance.bPublic)
		{
			const CClassProperties* pCompProperties = m_pProperties->GetComponentProperties(component.classComponentInstance.guid);
			if (pCompProperties)
			{
				bPublicPropertiesApplied = pCompProperties->Apply(component.classDesc, component.pComponent.get());
			}
		}
		if (!bPublicPropertiesApplied)
		{
			classComponentInstances[componentIdx].properties.Apply(component.classDesc, component.pComponent.get());
		}
	}

	if (m_pProperties)
	{
		for (const SRuntimeClassVariable& variable : m_pClass->GetVariables())
		{
			if (variable.bPublic)
			{
				if (!m_pProperties->ReadVariable(*m_scratchpad.Get(variable.pos), variable.guid))
				{
					SCHEMATYC_CORE_ERROR("Failed to initialize variable: class = %s, variable = %s", m_pClass->GetName(), variable.name.c_str());
					//return false;
					m_pProperties->AddVariable(variable.guid, variable.name.c_str(), *m_scratchpad.Get(variable.pos));
				}
			}
		}
	}

	return true;
}

void CObject::ExecuteConstructors(ESimulationMode simulationMode)
{
	CRuntimeParamMap params;
	for (const SRuntimeClassConstructor& classConstructor : m_pClass->GetConstructors())
	{
		ExecuteFunction(classConstructor.graphIdx, params, classConstructor.activationParams);
	}
}

bool CObject::CreateStateMachines()
{
	m_stateMachines.resize(m_pClass->GetStateMachines().size());

	// #SchematycTODO : Move to separate CreateStates function?
	const RuntimeClassStates& classStates = m_pClass->GetStates();
	const uint32 stateCount = classStates.size();
	m_states.resize(stateCount);

	ITimerSystem& timerSystem = gEnv->pSchematyc->GetTimerSystem();
	for (uint32 stateIdx = 0; stateIdx < stateCount; ++stateIdx)
	{
		const SRuntimeClassState& classState = classStates[stateIdx];
		SState& state = m_states[stateIdx];

		const uint32 stateTimerCount = classState.timers.size();
		state.timers.reserve(stateTimerCount);

		for (const SRuntimeClassStateTimer& classStateTimer : classState.timers)
		{
			state.timers.emplace_back(this, classStateTimer.guid, classStateTimer.params.flags);
			STimer& timer = state.timers.back();

			STimerParams timerParams = classStateTimer.params;
			timerParams.flags.Remove(ETimerFlags::AutoStart);

			timer.id = timerSystem.CreateTimer(timerParams, SCHEMATYC_MEMBER_DELEGATE(&STimer::Activate, timer));
		}
	}

	return true;
}

void CObject::StartStateMachines(ESimulationMode simulationMode)
{
	const RuntimeClassStateMachines& classStateMachines = m_pClass->GetStateMachines();
	for (uint32 stateMachineIdx = 0, stateMachineCount = m_stateMachines.size(); stateMachineIdx < stateMachineCount; ++stateMachineIdx)
	{
		StackRuntimeParamMap params;
		uint32 stateIdx = InvalidIdx;
		params.BindOutput(CUniqueId::FromUInt32(0), CAnyPtr(&stateIdx));

		ExecuteFunction(classStateMachines[stateMachineIdx].beginFunction, params);

		if (stateIdx != InvalidIdx)
		{
			ChangeState(stateMachineIdx, stateIdx);
		}
	}
}

void CObject::StopStateMachines()
{
	for (uint32 stateMachineIdx = 0, stateMachineCount = m_stateMachines.size(); stateMachineIdx < stateMachineCount; ++stateMachineIdx)
	{
		ChangeState(stateMachineIdx, InvalidIdx);
	}
}

void CObject::DestroyStateMachines()
{
	// #SchematycTODO : Move to separate DestroyStates function?
	ITimerSystem& timerSystem = gEnv->pSchematyc->GetTimerSystem();
	for (SState& state : m_states)
	{
		for (STimer& timer : state.timers)
		{
			timerSystem.DestroyTimer(timer.id);
			timer.id = TimerId::Invalid;
		}
	}
	// #SchematycTODO : m_stateMachines.clear()? m_states.clear()?
}

bool CObject::CreateComponents()
{
	const RuntimeClassComponentInstances& classComponentInstances = m_pClass->GetComponentInstances();
	const uint32 componentCount = classComponentInstances.size();

	m_components.reserve(componentCount);

	for (const SRuntimeClassComponentInstance& classComponentInstance : classComponentInstances)
	{
		const IEnvComponent* pEnvComponent = gEnv->pSchematyc->GetEnvRegistry().GetComponent(classComponentInstance.componentTypeGUID);
		if (pEnvComponent == nullptr)
		{
			return false;
		}

		m_components.emplace_back(classComponentInstance, pEnvComponent);

		if (m_components.back().pComponent == nullptr)
		{
			assert(0 && "Component create from pool failed");
			m_components.pop_back();

			return false;
		}
	}

	// Add Created components to the Entity
	if (IEntity* pEntity = GetEntity())
	{
		for (SComponent& component : m_components)
		{
			IEntityComponent* pParent = component.classComponentInstance.parentIdx != InvalidIdx ? m_components[component.classComponentInstance.parentIdx].pComponent.get() : nullptr;

			const CEntityComponentClassDesc& classDesc = static_cast<const CEntityComponentClassDesc&>(component.classDesc);

			EntityComponentFlags flags(EEntityComponentFlags::Schematyc);
			if (component.classComponentInstance.bPublic)
			{
				flags.Add(EEntityComponentFlags::SchematycEditable);
			}
			CTransformPtr transform;
			if (classDesc.GetComponentFlags().Check(IEntityComponent::EFlags::Transform))
			{
				flags.Add(EEntityComponentFlags::Transform);
				transform = component.classComponentInstance.transform;
			}

			IEntityComponent::SInitParams initParams(
			  pEntity,
			  component.classComponentInstance.guid,
			  component.classComponentInstance.name,
			  &classDesc,
			  flags,
			  pParent,
			  transform
			  );
			initParams.bNotInitialize = true;
			GetEntity()->AddComponent(component.classDesc.GetGUID(), component.pComponent, true, &initParams);
		}
	}

	return true;
}

bool CObject::InitComponents()
{
	IEntity* pEntity = GetEntity();
	if (!pEntity)
		return false;

	for (SComponent& component : m_components)
	{
		component.pComponent->Initialize();
	}

	return true;
}

void CObject::RunComponents(ESimulationMode simulationMode)
{
	for (SComponent& component : m_components)
	{
		component.pComponent->Run(simulationMode);
	}
}

void CObject::ShutdownComponents()
{
	if (GetEntity())
	{
		for (SComponent& component : m_components)
		{
			GetEntity()->RemoveComponent(component.pComponent.get());
		}
	}
}

void CObject::DestroyComponents()
{
	ShutdownComponents();
	m_components.clear();
}

bool CObject::CreateActions()
{
	const RuntimeActionDescs& actionDescs = m_pClass->GetActions();
	const uint32 actionCount = actionDescs.size();

	m_actions.reserve(actionCount);

	for (const SRuntimeActionDesc& actionDesc : actionDescs)
	{
		const IEnvAction* pEnvAction = gEnv->pSchematyc->GetEnvRegistry().GetAction(actionDesc.typeGUID);
		if (!pEnvAction)
		{
			return false;
		}

		CActionPtr pAction = pEnvAction->CreateFromPool();
		if (!pAction)
		{
			return false;
		}

		m_actions.emplace_back(pEnvAction->GetDesc(), actionDesc, pAction);
	}
	return true;
}

bool CObject::InitActions()
{
	for (SAction& action : m_actions)
	{
		SActionParams actionParams(action.runtimeDesc.guid, *this);
		action.ptr->PreInit(actionParams);
		if (!action.ptr->Init())
		{
			return false;
		}
	}
	return true;
}

void CObject::ShutdownActions()
{
	for (SAction& action : stl::reverse(m_actions))
	{
		if (action.bRunning)
		{
			action.ptr->Stop();
			action.bRunning = false;
		}
		action.ptr->Shutdown();
	}
}

void CObject::DestroyActions()
{
	for (SAction& action : stl::reverse(m_actions))
	{
		action.ptr.reset();
	}
	// #SchematycTODO : m_actions.clear()?
}

bool CObject::CreateTimers()
{
	const RuntimeClassTimers& classTimers = m_pClass->GetTimers();
	m_timers.reserve(classTimers.size());

	ITimerSystem& timerSystem = gEnv->pSchematyc->GetTimerSystem();
	for (const SRuntimeClassTimer& classTimer : classTimers)
	{
		m_timers.emplace_back(this, classTimer.guid, classTimer.params.flags);
		STimer& timer = m_timers.back();

		STimerParams timerParams = classTimer.params;
		timerParams.flags.Remove(ETimerFlags::AutoStart);

		timer.id = timerSystem.CreateTimer(timerParams, SCHEMATYC_MEMBER_DELEGATE(&STimer::Activate, timer));
	}
	return true;
}

void CObject::StartTimers(ESimulationMode simulationMode)
{
	if (simulationMode == ESimulationMode::Game)
	{
		ITimerSystem& timerSystem = gEnv->pSchematyc->GetTimerSystem();
		for (const STimer& timer : m_timers)
		{
			if (timer.flags.Check(ETimerFlags::AutoStart))
			{
				timerSystem.StartTimer(timer.id);
			}
		}
	}
}

void CObject::StopTimers()
{
	ITimerSystem& timerSystem = gEnv->pSchematyc->GetTimerSystem();
	for (const STimer& timer : m_timers)
	{
		timerSystem.StopTimer(timer.id);
	}
}

void CObject::DestroyTimers()
{
	ITimerSystem& timerSystem = gEnv->pSchematyc->GetTimerSystem();
	for (STimer& timer : m_timers)
	{
		timerSystem.DestroyTimer(timer.id);
		timer.id = TimerId::Invalid;
	}
	// #SchematycTODO : m_timers.clear()?
}

void CObject::RegisterForUpdate()
{
	if (m_pClass->CountSignalReceviers(GetTypeDesc<SUpdateSignal>().GetGUID()))
	{
		SUpdateParams updateParams(SCHEMATYC_MEMBER_DELEGATE(&CObject::Update, *this), m_connectionScope);
		updateParams.frequency = EUpdateFrequency::EveryFrame;
		// #SchematycTODO : Create an update filter?
		gEnv->pSchematyc->GetUpdateScheduler().Connect(updateParams);
	}
}

void CObject::ExecuteSignalReceivers(const SObjectSignal& signal)
{
	for (const SRuntimeClassSignalReceiver& classSignalReceiver : m_pClass->GetSignalReceivers())
	{
		bool bExecute = false;
		if (classSignalReceiver.signalGUID == signal.typeGUID)
		{
			// #SchematycTODO : How can we optimize this query?
			if (GUID::IsEmpty(classSignalReceiver.senderGUID))
			{
				bExecute = true;
			}
			else if (classSignalReceiver.senderGUID == signal.senderGUID)
			{
				bExecute = true;
			}
			else if (GUID::IsEmpty(signal.senderGUID))
			{
				SCHEMATYC_CORE_CRITICAL_ERROR("Signal sender guid expected!");
			}
		}

		if (bExecute)
		{
			StackRuntimeParamMap params(signal.params); // #SchematycTODO : How can we avoid copying signal parameters?
			ExecuteFunction(classSignalReceiver.graphIdx, params, classSignalReceiver.activationParams);
		}
	}
}

void CObject::StartStateTimers(uint32 stateMachineIdx)
{
	const uint32 stateIdx = m_stateMachines[stateMachineIdx].stateIdx;
	if (stateIdx != InvalidIdx)
	{
		ITimerSystem& timerSystem = gEnv->pSchematyc->GetTimerSystem();
		for (const STimer& timer : m_states[stateIdx].timers)
		{
			if (timer.flags.Check(ETimerFlags::AutoStart))
			{
				timerSystem.StartTimer(timer.id);
			}
		}
	}
}

void CObject::StopStateTimers(uint32 stateMachineIdx)
{
	const uint32 stateIdx = m_stateMachines[stateMachineIdx].stateIdx;
	if (stateIdx != InvalidIdx)
	{
		ITimerSystem& timerSystem = gEnv->pSchematyc->GetTimerSystem();
		for (const STimer& timer : m_states[stateIdx].timers)
		{
			timerSystem.StopTimer(timer.id);
		}
	}
}

void CObject::ExecuteStateSignalReceivers(uint32 stateMachineIdx, const SObjectSignal& signal)
{
	const uint32 stateIdx = m_stateMachines[stateMachineIdx].stateIdx;
	if (stateIdx != InvalidIdx)
	{
		const RuntimeClassStates& classStates = m_pClass->GetStates();
		for (const SRuntimeClassStateSignalReceiver& classStateSignalReceiver : classStates[stateIdx].signalReceivers)
		{
			bool bExecute = false;
			if (classStateSignalReceiver.signalGUID == signal.typeGUID)
			{
				// #SchematycTODO : How can we optimize this query?
				if (GUID::IsEmpty(classStateSignalReceiver.senderGUID))
				{
					bExecute = true;
				}
				else if (classStateSignalReceiver.senderGUID == signal.senderGUID)
				{
					bExecute = true;
				}
				else if (GUID::IsEmpty(signal.senderGUID))
				{
					SCHEMATYC_CORE_CRITICAL_ERROR("Signal sender guid expected!");
				}
			}

			if (bExecute)
			{
				StackRuntimeParamMap params(signal.params); // #SchematycTODO : How can we avoid copying signal parameters?
				ExecuteFunction(classStateSignalReceiver.graphIdx, params, classStateSignalReceiver.activationParams);
			}
		}
	}
}

bool CObject::EvaluateStateTransitions(uint32 stateMachineIdx, const SObjectSignal& signal)
{
	const uint32 stateIdx = m_stateMachines[stateMachineIdx].stateIdx;
	if (stateIdx != InvalidIdx)
	{
		const RuntimeClassStates& classStates = m_pClass->GetStates();
		for (const SRuntimeClassStateTransition& classStateTransition : classStates[stateIdx].transitions)
		{
			if (classStateTransition.signalGUID == signal.typeGUID)
			{
				StackRuntimeParamMap params(signal.params); // #SchematycTODO : How can we avoid copying signal parameters?
				uint32 stateIdx = InvalidIdx;
				params.BindOutput(CUniqueId::FromUInt32(0), CAnyPtr(&stateIdx));

				ExecuteFunction(classStateTransition.graphIdx, params, classStateTransition.activationParams);

				if (stateIdx != InvalidIdx)
				{
					ChangeState(stateMachineIdx, stateIdx);
					return true;
				}
			}
		}
	}
	return false;
}

void CObject::ChangeState(uint32 stateMachineIdx, uint32 stateIdx)
{
	StopStateTimers(stateMachineIdx);
	ExecuteStateSignalReceivers(stateMachineIdx, SObjectSignal::FromSignalClass(SStopSignal()));
	m_stateMachines[stateMachineIdx].stateIdx = stateIdx;
	ExecuteStateSignalReceivers(stateMachineIdx, SObjectSignal::FromSignalClass(SStartSignal()));
	StartStateTimers(stateMachineIdx);
}

void CObject::ExecuteFunction(const SRuntimeFunction& function, CRuntimeParamMap& params)
{
	if (function.graphIdx != InvalidIdx)
	{
		m_graphs[function.graphIdx].Execute(this, params, function.activationParams);
	}
}

void CObject::ExecuteFunction(uint32 graphIdx, CRuntimeParamMap& params, SRuntimeActivationParams activationParams)
{
	m_graphs[graphIdx].Execute(this, params, activationParams);
}

void CObject::ProcessSignalQueue()
{
	while (!m_signalQueue.empty())
	{
		SQueuedObjectSignal queuedSignal = m_signalQueue.front();
		m_signalQueue.pop_front();
		ProcessSignal(queuedSignal.signal);
	}
}

} // Schematyc
