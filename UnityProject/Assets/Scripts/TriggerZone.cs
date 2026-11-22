using System;
using UnityEngine;

public class TriggerZone : MonoBehaviour
{
    [Header("Cooldown Settings")]
    [Tooltip("Time in seconds before this trigger zone can activate again.")]
    [SerializeField] private float cooldownTime = 1f;

    [SerializeField] private int areaCode = 0;
    private bool _isCooldown;
    private bool _isPlayerInside;
    private float _cooldownTimer;
    private AkEvent _changeAreaEvent;

    private void OnTriggerEnter(Collider other)
    {
        if (_isCooldown) return;
        if (_isPlayerInside) return;

        _isPlayerInside = true;
        OnPlayerEntered(other);

        _isCooldown = true;
        _cooldownTimer = cooldownTime;
    }

    private void OnTriggerExit(Collider other)
    {
        _isPlayerInside = false;
    }

    private void Update()
    {
        if (!_isCooldown) return;

        _cooldownTimer -= Time.deltaTime;
        if (_cooldownTimer <= 0f)
        {
            _isCooldown = false;
            _cooldownTimer = 0f;
        }
    }

    private void OnPlayerEntered(Collider other)
    {
        // Placeholder: implement your action here.
        Debug.Log("Player entered trigger zone: " + gameObject.name);
    }
}
