// Author: Lucas Vilas-Boas
// Year: 2022
// Repo: https://github.com/lucoiso/UEProject_Elementus

#include "GAS/System/PEGameplayAbility.h"
#include "AbilitySystemGlobals.h"
#include "GAS/System/PEAbilitySystemComponent.h"
#include "GAS/Tasks/PESpawnProjectile_Task.h"
#include "Abilities/Tasks/AbilityTask_WaitGameplayEvent.h"
#include "Abilities/Tasks/AbilityTask_PlayMontageAndWait.h"
#include "Abilities/Tasks/AbilityTask_WaitConfirmCancel.h"
#include "Abilities/Tasks/AbilityTask_WaitCancel.h"
#include "Abilities/Tasks/AbilityTask_WaitGameplayTag.h"
#include "Abilities/Tasks/AbilityTask_WaitTargetData.h"
#include "Abilities/Tasks/AbilityTask_SpawnActor.h"
#include "Abilities/GameplayAbilityTargetActor_SingleLineTrace.h"
#include "Abilities/GameplayAbilityTargetActor_GroundTrace.h"
#include "Actors/Character/PECharacter.h"
#include "Actors/World/PEProjectileActor.h"
#include "GameplayEffect.h"

UPEGameplayAbility::UPEGameplayAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer),
	  AbilityMaxRange(0),
	  bIgnoreCost(false),
	  bIgnoreCooldown(false),
	  bWaitCancel(true),
	  AbilityActiveTime(0),
	  bEndAbilityAfterActiveTime(false)
{
	ActivationBlockedTags.AddTag(FGameplayTag::RequestGameplayTag("State.Dead"));
	ActivationBlockedTags.AddTag(FGameplayTag::RequestGameplayTag("State.Stunned"));

	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;

	bIsCancelable = true;
}

void UPEGameplayAbility::OnGiveAbility(const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilitySpec& Spec)
{
	ABILITY_VLOG(this, Display, TEXT("Ability %s given to %s."), *GetName(), *ActorInfo->AvatarActor->GetName());

	Super::OnGiveAbility(ActorInfo, Spec);

	// If the ability failed to activate on granting, will notify the ability system component
	if (bAutoActivateOnGrant && !ActorInfo->AbilitySystemComponent->TryActivateAbility(Spec.Handle))
	{
		const TArray<FGameplayTag>& FailureTags =
		{
			FGameplayTag::RequestGameplayTag("GameplayAbility.Fail.OnGive"),
			FGameplayTag::RequestGameplayTag("GameplayAbility.Fail.TryActivate")
		};
		const FGameplayTagContainer& FailureContainer = FGameplayTagContainer::CreateFromArray(FailureTags);
		ActorInfo->AbilitySystemComponent->NotifyAbilityFailed(Spec.Handle, this, FailureContainer);
	}
}

void UPEGameplayAbility::PreActivate(const FGameplayAbilitySpecHandle Handle,
                                     const FGameplayAbilityActorInfo* ActorInfo,
                                     const FGameplayAbilityActivationInfo ActivationInfo,
                                     FOnGameplayAbilityEnded::FDelegate* OnGameplayAbilityEndedDelegate,
                                     const FGameplayEventData* TriggerEventData)
{
	ABILITY_VLOG(this, Display, TEXT("Trying pre-activate %s ability."), *GetName());

	Super::PreActivate(Handle, ActorInfo, ActivationInfo, OnGameplayAbilityEndedDelegate, TriggerEventData);

	// Cancel the ability if can't commit cost or cooldown. Also cancel if has not auth or is not predicted
	if (const bool bCanCommit = CommitCheck(Handle, ActorInfo, ActivationInfo);
		!bCanCommit || !HasAuthorityOrPredictionKey(ActorInfo, &ActivationInfo))
	{
		TArray FailureTags =
		{
			FGameplayTag::RequestGameplayTag("GameplayAbility.Fail.PreActivate"),
		};

		if (!bCanCommit)
		{
			FailureTags.Add(FGameplayTag::RequestGameplayTag("GameplayAbility.Fail.CommitCheck"));
		}
		if (!HasAuthorityOrPredictionKey(ActorInfo, &ActivationInfo))
		{
			FailureTags.Add(FGameplayTag::RequestGameplayTag("GameplayAbility.Fail.HasAuthorityOrPredictionKey"));
		}
		if (GetCooldownTimeRemaining() > 0.f)
		{
			FailureTags.Add(FGameplayTag::RequestGameplayTag("GameplayAbility.Fail.Cooldown"));
		}

		const FGameplayTagContainer& FailureContainer = FGameplayTagContainer::CreateFromArray(FailureTags);
		ActorInfo->AbilitySystemComponent->NotifyAbilityFailed(Handle, this, FailureContainer);

		CancelAbility(Handle, ActorInfo, ActivationInfo, true);
	}

	ActivationBlockedTags.AppendTags(AbilityTags);

	// Auto cancel can only be called on instantiated abilities. Non-Instantiated abilities can't handle tasks
	if (IsInstantiated())
	{
		UAbilityTask_WaitGameplayTagAdded* WaitDeadTagAddedTask =
			UAbilityTask_WaitGameplayTagAdded::WaitGameplayTagAdd(this, FGameplayTag::RequestGameplayTag("State.Dead"));

		WaitDeadTagAddedTask->Added.AddDynamic(this, &UPEGameplayAbility::K2_EndAbility);
		WaitDeadTagAddedTask->ReadyForActivation();

		UAbilityTask_WaitGameplayTagAdded* WaitStunTagAddedTask =
			UAbilityTask_WaitGameplayTagAdded::WaitGameplayTagAdd(
				this, FGameplayTag::RequestGameplayTag("State.Stunned"));

		WaitStunTagAddedTask->Added.AddDynamic(this, &UPEGameplayAbility::K2_EndAbility);
		WaitStunTagAddedTask->ReadyForActivation();

		if (CanBeCanceled() && bWaitCancel)
		{
			ActivateWaitCancelInputTask();
		}
	}

	// If the ability is time based, will cancel after active time
	if (bEndAbilityAfterActiveTime)
	{
		FTimerDelegate TimerDelegate;
		TimerDelegate.BindLambda([&]() -> void
		{
			if (IsValid(this) && IsActive())
			{
				EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
			}
		});

		ActorInfo->AvatarActor->GetWorld()->GetTimerManager().SetTimer(CancelationTimerHandle, TimerDelegate,
		                                                               AbilityActiveTime,
		                                                               false);
	}
}

void UPEGameplayAbility::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
                                         const FGameplayAbilityActorInfo* ActorInfo,
                                         const FGameplayAbilityActivationInfo ActivationInfo,
                                         const FGameplayEventData* TriggerEventData)
{
	ABILITY_VLOG(this, Display, TEXT("%s ability successfully activated."), *GetName());

	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);
}

void UPEGameplayAbility::EndAbility(const FGameplayAbilitySpecHandle Handle,
                                    const FGameplayAbilityActorInfo* ActorInfo,
                                    const FGameplayAbilityActivationInfo ActivationInfo,
                                    const bool bReplicateEndAbility,
                                    const bool bWasCancelled)
{
	if (!ActorInfo && IsInstantiated())
	{
		ActorInfo = GetCurrentActorInfo();
	}

	ABILITY_VLOG(this, Display, TEXT("Ending %s ability."), *GetName());

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);

	// Remove active time based cost effects
	if (IsValid(GetCostGameplayEffect())
		&& GetCostGameplayEffect()->DurationPolicy == EGameplayEffectDurationType::Infinite)
	{
		ActorInfo->AbilitySystemComponent->RemoveActiveGameplayEffectBySourceEffect(
			GetCostGameplayEffect()->GetClass(), ActorInfo->AbilitySystemComponent.Get());
	}

	// Remove active time based buff/debuff effects from self
	if (AbilityActiveTime <= 0.f)
	{
		for (const FGameplayEffectGroupedData& EffectGroup : SelfAbilityEffects)
		{
			ActorInfo->AbilitySystemComponent->RemoveActiveGameplayEffectBySourceEffect(
				EffectGroup.EffectClass, ActorInfo->AbilitySystemComponent.Get());
		}
	}

	// If auto cancel by time is active, try to invalidate the timer handle and finish the timer
	if (CancelationTimerHandle.IsValid())
	{
		CancelationTimerHandle.Invalidate();
	}

	// Remove extra tags that were given by this ability
	if (UAbilitySystemComponent* Comp = ActorInfo->AbilitySystemComponent.Get())
	{
		Comp->RemoveLooseGameplayTags(AbilityExtraTags);
	}
}

bool UPEGameplayAbility::CommitAbilityCooldown(const FGameplayAbilitySpecHandle Handle,
                                               const FGameplayAbilityActorInfo* ActorInfo,
                                               const FGameplayAbilityActivationInfo ActivationInfo,
                                               const bool ForceCooldown,
                                               OUT FGameplayTagContainer* OptionalRelevantTags)
{
	return bIgnoreCooldown
		       ? true
		       : Super::CommitAbilityCooldown(Handle, ActorInfo, ActivationInfo, ForceCooldown, OptionalRelevantTags);
}

bool UPEGameplayAbility::CommitAbilityCost(const FGameplayAbilitySpecHandle Handle,
                                           const FGameplayAbilityActorInfo* ActorInfo,
                                           const FGameplayAbilityActivationInfo ActivationInfo,
                                           OUT FGameplayTagContainer* OptionalRelevantTags)
{
	return bIgnoreCost
		       ? true
		       : Super::CommitAbilityCost(Handle, ActorInfo, ActivationInfo, OptionalRelevantTags);
}

void UPEGameplayAbility::CommitExecute(const FGameplayAbilitySpecHandle Handle,
                                       const FGameplayAbilityActorInfo* ActorInfo,
                                       const FGameplayAbilityActivationInfo ActivationInfo)
{
	if (!bIgnoreCooldown)
	{
		ApplyCooldown(Handle, ActorInfo, ActivationInfo);
	}

	if (!bIgnoreCost)
	{
		ApplyCost(Handle, ActorInfo, ActivationInfo);
	}
}

void UPEGameplayAbility::ActivateGameplayCues(const FGameplayTag GameplayCueTag,
                                              FGameplayCueParameters Parameters,
                                              UAbilitySystemComponent* SourceAbilitySystem)
{
	if (SourceAbilitySystem == nullptr)
	{
		SourceAbilitySystem = GetAbilitySystemComponentFromActorInfo_Checked();
	}

	if (GameplayCueTag.IsValid())
	{
		ABILITY_VLOG(this, Display, TEXT("Activating %s ability associated Gameplay Cues with Tag %s."),
		             *GetName(), *GameplayCueTag.ToString());

		Parameters.AbilityLevel = GetAbilityLevel();
		SourceAbilitySystem->GetOwnedGameplayTags(Parameters.AggregatedSourceTags);
		Parameters.Instigator = SourceAbilitySystem->GetAvatarActor();
		Parameters.EffectContext = SourceAbilitySystem->MakeEffectContext();
		Parameters.SourceObject = SourceAbilitySystem;

		SourceAbilitySystem->AddGameplayCue(GameplayCueTag, Parameters);
		TrackedGameplayCues.Add(GameplayCueTag);
	}
	else
	{
		ABILITY_VLOG(this, Warning, TEXT("Ability %s failed to activate Gameplay Cue."), *GetName());
	}
}

void UPEGameplayAbility::BP_ApplyAbilityEffectsToSelf()
{
	check(CurrentActorInfo);

	ApplyAbilityEffectsToSelf(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo);
}

void UPEGameplayAbility::ApplyAbilityEffectsToSelf(const FGameplayAbilitySpecHandle Handle,
                                                   const FGameplayAbilityActorInfo* ActorInfo,
                                                   const FGameplayAbilityActivationInfo ActivationInfo)
{
	ABILITY_VLOG(this, Display, TEXT("Applying %s ability effects to owner."), *GetName());

	for (const FGameplayEffectGroupedData& EffectGroup : SelfAbilityEffects)
	{
		const FGameplayEffectSpecHandle& SpecHandle =
			MakeOutgoingGameplayEffectSpec(Handle, ActorInfo, ActivationInfo, EffectGroup.EffectClass);

		for (const TPair<FGameplayTag, float>& StackedData : EffectGroup.SetByCallerStackedData)
		{
			SpecHandle.Data.Get()->SetSetByCallerMagnitude(StackedData.Key, StackedData.Value);
		}

		if (SpecHandle.IsValid())
		{
			ApplyGameplayEffectSpecToOwner(Handle, ActorInfo, ActivationInfo, SpecHandle);
		}
	}
}

void UPEGameplayAbility::BP_RemoveAbilityEffectsFromSelf()
{
	check(CurrentActorInfo);

	RemoveAbilityEffectsFromSelf(CurrentActorInfo);
}

void UPEGameplayAbility::RemoveAbilityEffectsFromSelf(const FGameplayAbilityActorInfo* ActorInfo)
{
	ABILITY_VLOG(this, Display, TEXT("Removing %s ability effects from owner."), *GetName());

	for (const FGameplayEffectGroupedData& EffectGroup : SelfAbilityEffects)
	{
		FGameplayEffectQuery Query;
		Query.EffectDefinition = EffectGroup.EffectClass;

		ActorInfo->AbilitySystemComponent.Get()->RemoveActiveEffects(Query);
	}
}

void UPEGameplayAbility::BP_ApplyAbilityEffectsToTarget(const FGameplayAbilityTargetDataHandle TargetDataHandle)
{
	check(CurrentActorInfo);

	ApplyAbilityEffectsToTarget(TargetDataHandle, CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo);
}

void UPEGameplayAbility::ApplyAbilityEffectsToTarget(const FGameplayAbilityTargetDataHandle TargetDataHandle,
                                                     const FGameplayAbilitySpecHandle Handle,
                                                     const FGameplayAbilityActorInfo* ActorInfo,
                                                     const FGameplayAbilityActivationInfo ActivationInfo)
{
	ABILITY_VLOG(this, Display, TEXT("Applying %s ability effects to targets."), *GetName());

	for (const FGameplayEffectGroupedData& EffectGroup : TargetAbilityEffects)
	{
		const FGameplayEffectSpecHandle& SpecHandle =
			MakeOutgoingGameplayEffectSpec(Handle, ActorInfo, ActivationInfo, EffectGroup.EffectClass);

		for (const TPair<FGameplayTag, float>& StackedData : EffectGroup.SetByCallerStackedData)
		{
			SpecHandle.Data.Get()->SetSetByCallerMagnitude(StackedData.Key, StackedData.Value);
		}

		if (SpecHandle.IsValid())
		{
			ApplyGameplayEffectSpecToTarget(Handle, ActorInfo, ActivationInfo, SpecHandle, TargetDataHandle);
		}
	}
}

void UPEGameplayAbility::BP_SpawnProjectileWithTargetEffects(
	const TSubclassOf<APEProjectileActor> ProjectileClass,
	const FTransform ProjectileTransform, const FVector ProjectileFireDirection)
{
	check(CurrentActorInfo);

	SpawnProjectileWithTargetEffects(ProjectileClass, ProjectileTransform, ProjectileFireDirection,
	                                 CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo);
}

void UPEGameplayAbility::SpawnProjectileWithTargetEffects(const TSubclassOf<APEProjectileActor> ProjectileClass,
                                                          const FTransform ProjectileTransform,
                                                          const FVector ProjectileFireDirection,
                                                          [[maybe_unused]] const FGameplayAbilitySpecHandle,
                                                          [[maybe_unused]] const FGameplayAbilityActorInfo*,
                                                          [[maybe_unused]] const FGameplayAbilityActivationInfo)
{
	UPESpawnProjectile_Task* PESpawnProjectile_Task =
		UPESpawnProjectile_Task::SpawnProjectile(this, ProjectileClass, ProjectileTransform,
		                                         ProjectileFireDirection, TargetAbilityEffects);

	PESpawnProjectile_Task->OnProjectileSpawn.AddDynamic(this, &UPEGameplayAbility::SpawnProjectile_Callback);
	PESpawnProjectile_Task->OnSpawnFailed.AddDynamic(this, &UPEGameplayAbility::SpawnProjectile_Callback);

	PESpawnProjectile_Task->ReadyForActivation();
}

void UPEGameplayAbility::RemoveCooldownEffect(UAbilitySystemComponent* SourceAbilitySystem) const
{
	if (IsValid(GetCooldownGameplayEffect()))
	{
		ABILITY_VLOG(this, Display, TEXT("Removing %s ability cooldown."), *GetName());

		FGameplayTagContainer CooldownEffectTags;
		GetCooldownGameplayEffect()->GetOwnedGameplayTags(CooldownEffectTags);
		SourceAbilitySystem->RemoveActiveEffectsWithAppliedTags(CooldownEffectTags);
	}
}

void UPEGameplayAbility::ActivateWaitMontageTask(const FName MontageSection, const float Rate,
                                                 const bool bRandomSection, const bool bStopsWhenAbilityEnds)
{
	FName MontageSectionName = MontageSection;

	if (bRandomSection)
	{
		MontageSectionName =
			AbilityAnimation->GetSectionName(FMath::RandRange(0, AbilityAnimation->CompositeSections.Num()));
	}

	UAbilityTask_PlayMontageAndWait* AbilityTask_PlayMontageAndWait =
		UAbilityTask_PlayMontageAndWait::CreatePlayMontageAndWaitProxy(this, "WaitMontageTask",
		                                                               AbilityAnimation, Rate,
		                                                               MontageSectionName, bStopsWhenAbilityEnds);

	AbilityTask_PlayMontageAndWait->OnBlendOut.AddDynamic(this, &UPEGameplayAbility::WaitMontage_Callback);
	AbilityTask_PlayMontageAndWait->OnInterrupted.AddDynamic(this, &UPEGameplayAbility::K2_EndAbility);
	AbilityTask_PlayMontageAndWait->OnCancelled.AddDynamic(this, &UPEGameplayAbility::K2_CancelAbility);

	AbilityTask_PlayMontageAndWait->ReadyForActivation();
}

void UPEGameplayAbility::ActivateWaitTargetDataTask(
	const TEnumAsByte<EGameplayTargetingConfirmation::Type> TargetingConfirmation,
	const TSubclassOf<AGameplayAbilityTargetActor_Trace> TargetActorClass,
	FTargetActorSpawnParams TargetParameters)
{
	if constexpr (&TargetParameters.StartLocation == nullptr)
	{
		TargetParameters.StartLocation = MakeTargetLocationInfoFromOwnerActor();
	}

	TargetParameters.Range = AbilityMaxRange;

	UAbilityTask_WaitTargetData* AbilityTask_WaitTargetData =
		UAbilityTask_WaitTargetData::WaitTargetData(this, "WaitTargetDataTask",
		                                            TargetingConfirmation,
		                                            TargetActorClass);

	AbilityTask_WaitTargetData->Cancelled.AddDynamic(this, &UPEGameplayAbility::WaitTargetData_Callback);
	AbilityTask_WaitTargetData->ValidData.AddDynamic(this, &UPEGameplayAbility::WaitTargetData_Callback);

	// Initialize the spawning task with the TargetActor
	if (AGameplayAbilityTargetActor* TargetActor = nullptr;
		AbilityTask_WaitTargetData->BeginSpawningActor(this, TargetActorClass, TargetActor))
	{
		TargetActor->StartLocation = TargetParameters.StartLocation;
		TargetActor->ReticleClass = TargetParameters.ReticleClass;
		TargetActor->ReticleParams = TargetParameters.ReticleParams;
		TargetActor->bDebug = TargetParameters.bDebug;

		FGameplayTargetDataFilterHandle FilterHandle;
		FilterHandle.Filter = MakeShared<FGameplayTargetDataFilter>(TargetParameters.TargetFilter);
		TargetActor->Filter = FilterHandle;

		// Check if TargetActorClass is child of AGameplayAbilityTargetActor_Trace and add values to class params
		if (TargetActorClass.Get()->IsChildOf<AGameplayAbilityTargetActor_Trace>())
		{
			AGameplayAbilityTargetActor_Trace* TraceObj = Cast<AGameplayAbilityTargetActor_Trace>(TargetActor);

			TraceObj->MaxRange = TargetParameters.Range;
			TraceObj->bTraceAffectsAimPitch = TargetParameters.bTraceAffectsAimPitch;

			// Check if TargetActorClass is child of AGameplayAbilityTargetActor_GroundTrace
			// and add values to class params
			if (TargetActorClass.Get()->IsChildOf<AGameplayAbilityTargetActor_GroundTrace>())
			{
				AGameplayAbilityTargetActor_GroundTrace* GroundTraceObj =
					Cast<AGameplayAbilityTargetActor_GroundTrace>(TargetActor);

				GroundTraceObj->CollisionRadius = TargetParameters.Radius;
				GroundTraceObj->CollisionHeight = TargetParameters.Height;
			}
		}

		AbilityTask_WaitTargetData->FinishSpawningActor(this, TargetActor);
		TargetActor->bDestroyOnConfirmation = TargetParameters.bDestroyOnConfirmation;
		AbilityTask_WaitTargetData->ReadyForActivation();
	}

	// If Targeting is different than Instant, add the aiming tag and start waiting for confirmation
	if (AbilityTask_WaitTargetData->IsActive()
		&& TargetingConfirmation != EGameplayTargetingConfirmation::Instant)
	{
		UAbilitySystemComponent* Comp = GetAbilitySystemComponentFromActorInfo_Checked();

		const FGameplayTag AddTag_Aiming = FGameplayTag::RequestGameplayTag(FName("State.Aiming"));
		const FGameplayTag AddTag_Confirmation = FGameplayTag::RequestGameplayTag(FName("State.WaitingConfirm"));

		Comp->AddLooseGameplayTag(AddTag_Aiming);
		Comp->AddLooseGameplayTag(AddTag_Confirmation);

		AbilityExtraTags.AddTag(AddTag_Aiming);
		AbilityExtraTags.AddTag(AddTag_Confirmation);
	}
}

void UPEGameplayAbility::ActivateWaitConfirmInputTask()
{
	// Add extra tag to the ability system component to tell that we are waiting for confirm input
	UAbilitySystemComponent* Comp = GetAbilitySystemComponentFromActorInfo_Checked();
	if (const FGameplayTag AddTag = FGameplayTag::RequestGameplayTag(FName("State.WaitingConfirm"));
		!AbilityExtraTags.HasTag(AddTag))
	{
		Comp->AddLooseGameplayTag(AddTag);
		AbilityExtraTags.AddTag(AddTag);
	}

	UAbilityTask_WaitConfirmCancel* AbilityTask_WaitConfirm =
		UAbilityTask_WaitConfirmCancel::WaitConfirmCancel(this);

	AbilityTask_WaitConfirm->OnConfirm.AddDynamic(this, &UPEGameplayAbility::WaitConfirmInput_Callback);

	// Canceling is already binded by ActivateWaitCancelInputTask()
	// We will only use it to re-activate this task if bWaitCancel is false
	// Because this WaitConfirmCancel task ends independly if Cancel or Confirm is pressed
	if (!bWaitCancel)
	{
		AbilityTask_WaitConfirm->OnCancel.AddDynamic(this, &UPEGameplayAbility::ActivateWaitConfirmInputTask);
	}
	AbilityTask_WaitConfirm->ReadyForActivation();
}

void UPEGameplayAbility::ActivateWaitCancelInputTask()
{
	// Add extra tag to the ability system component to tell that we are waiting for cancel input
	UAbilitySystemComponent* Comp = GetAbilitySystemComponentFromActorInfo_Checked();
	if (const FGameplayTag AddTag = FGameplayTag::RequestGameplayTag(FName("State.WaitingCancel"));
		!AbilityExtraTags.HasTag(AddTag))
	{
		Comp->AddLooseGameplayTag(AddTag);
		AbilityExtraTags.AddTag(AddTag);
	}

	UAbilityTask_WaitCancel* AbilityTask_WaitCancel =
		UAbilityTask_WaitCancel::WaitCancel(this);

	AbilityTask_WaitCancel->OnCancel.AddDynamic(this, &UPEGameplayAbility::WaitCancelInput_Callback);
	AbilityTask_WaitCancel->ReadyForActivation();
}

void UPEGameplayAbility::ActivateWaitAddedTagTask(const FGameplayTag Tag)
{
	UAbilityTask_WaitGameplayTagAdded* AbilityTask_WaitGameplayTagAdded =
		UAbilityTask_WaitGameplayTagAdded::WaitGameplayTagAdd(this, Tag);

	AbilityTask_WaitGameplayTagAdded->Added.AddDynamic(this, &UPEGameplayAbility::WaitAddedTag_Callback);
	AbilityTask_WaitGameplayTagAdded->ReadyForActivation();
}

void UPEGameplayAbility::ActivateWaitRemovedTagTask(const FGameplayTag Tag)
{
	UAbilityTask_WaitGameplayTagRemoved* AbilityTask_WaitGameplayTagRemoved =
		UAbilityTask_WaitGameplayTagRemoved::WaitGameplayTagRemove(this, Tag);

	AbilityTask_WaitGameplayTagRemoved->Removed.AddDynamic(this, &UPEGameplayAbility::WaitRemovedTag_Callback);
	AbilityTask_WaitGameplayTagRemoved->ReadyForActivation();
}

void UPEGameplayAbility::ActivateWaitGameplayEventTask(const FGameplayTag EventTag)
{
	UAbilityTask_WaitGameplayEvent* AbilityTask_WaitGameplayEvent =
		UAbilityTask_WaitGameplayEvent::WaitGameplayEvent(this, EventTag);

	AbilityTask_WaitGameplayEvent->EventReceived.AddDynamic(this, &UPEGameplayAbility::WaitGameplayEvent_Callback);
	AbilityTask_WaitGameplayEvent->ReadyForActivation();
}

void UPEGameplayAbility::ActivateSpawnActorTask(const FGameplayAbilityTargetDataHandle TargetDataHandle,
                                                const TSubclassOf<AActor> ActorClass)
{
	UAbilityTask_SpawnActor* AbilityTask_SpawnActor =
		UAbilityTask_SpawnActor::SpawnActor(this, TargetDataHandle, ActorClass);

	AbilityTask_SpawnActor->DidNotSpawn.AddDynamic(this, &UPEGameplayAbility::SpawnActor_Callback);
	AbilityTask_SpawnActor->Success.AddDynamic(this, &UPEGameplayAbility::SpawnActor_Callback);
	AbilityTask_SpawnActor->ReadyForActivation();
}

void UPEGameplayAbility::WaitCancelInput_Callback()
{
	if (CanBeCanceled())
	{
		CancelAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true);
	}
}
