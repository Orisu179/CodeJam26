using AK.Wwise;
using UnityEngine;
using UnityEngine.InputSystem;

public class PlayerController : MonoBehaviour
{
    [Header("Movement Settings")] [SerializeField]
    private float moveSpeed = 5f;

    [Header("Mouse Look Settings")] [SerializeField]
    private float mouseSensitivity = 2f;

    [SerializeField] private float minPitch = -80f;
    [SerializeField] private float maxPitch = 80f;

    [Header("Wwise Events")] [SerializeField]
    private AK.Wwise.Event footstepPlayEvent;

    [SerializeField] private AK.Wwise.Event footstepStopEvent;

    [SerializeField] private AK.Wwise.Event musicPlayEvent;

    [SerializeField] private AK.Wwise.Event musicStopEvent;
    [SerializeField] private AK.Wwise.RTPC dryWetRTPC;

    private float dryWet;
    private Vector2 moveInput;
    private Transform cameraTransform;
    private float pitchAngle;
    private bool isMoving;
    private bool isMusicPlaying;

    // Input actions
    private InputAction moveAction;
    private InputAction lookAction;
    private InputAction musicAction;
    private InputAction increaseDryWetAction;
    private InputAction decreaseDryWetAction;

    void Awake()
    {
        AkUnitySoundEngine.RegisterGameObj(gameObject);
        // Find the camera in a child transform
        Transform childCamera = transform.GetChild(0);
        if (childCamera != null && childCamera.GetComponent<Camera>() != null)
        {
            cameraTransform = childCamera;
        }
        else
        {
            // Fallback: try to find any Camera component in children
            Camera childCam = GetComponentInChildren<Camera>();
            if (childCam != null)
            {
                cameraTransform = childCam.transform;
            }
        }

        if (cameraTransform == null)
        {
            Debug.LogWarning("PlayerController: No camera found as child. Mouse look will be disabled.");
        }

        // Create input actions programmatically
        moveAction = new InputAction("Move", type: InputActionType.Value);
        moveAction.AddCompositeBinding("Dpad")
            .With("Up", "<Keyboard>/w")
            .With("Down", "<Keyboard>/s")
            .With("Left", "<Keyboard>/a")
            .With("Right", "<Keyboard>/d");

        // Also support gamepad stick
        moveAction.AddBinding("<Gamepad>/leftStick");

        moveAction.Enable();
        moveAction.performed += OnMove;
        moveAction.canceled += OnMove;

        // Mouse look action
        lookAction = new InputAction("Look", type: InputActionType.Value);
        lookAction.AddBinding("<Mouse>/delta");
        lookAction.Enable();
        lookAction.performed += OnLook;
        lookAction.canceled += OnLook;

        // Music toggle action (P key)
        musicAction = new InputAction("Music", type: InputActionType.Button);
        musicAction.AddBinding("<Keyboard>/p");
        musicAction.Enable();
        musicAction.performed += OnMusicToggle;

        // Dry/Wet increase action (0 key)
        increaseDryWetAction = new InputAction("IncreaseDryWet", type: InputActionType.Button);
        increaseDryWetAction.AddBinding("<Keyboard>/0");
        increaseDryWetAction.Enable();
        increaseDryWetAction.performed += OnIncreaseDryWet;

        // Dry/Wet decrease action (9 key)
        decreaseDryWetAction = new InputAction("DecreaseDryWet", type: InputActionType.Button);
        decreaseDryWetAction.AddBinding("<Keyboard>/9");
        decreaseDryWetAction.Enable();
        decreaseDryWetAction.performed += OnDecreaseDryWet;
    }

    void Update()
    {
        // Apply movement based on the latest input, relative to camera direction
        if (moveInput != Vector2.zero)
        {
            Vector3 movement = GetCameraRelativeMovement(moveInput);
            transform.Translate(movement * moveSpeed * Time.deltaTime, Space.World);
        }
    }

    private Vector3 GetCameraRelativeMovement(Vector2 input)
    {
        if (cameraTransform != null)
        {
            // Get camera's forward and right, ignoring Y (flatten to horizontal plane)
            Vector3 forward = cameraTransform.forward;
            Vector3 right = cameraTransform.right;
            forward.y = 0f;
            right.y = 0f;
            forward.Normalize();
            right.Normalize();

            // Calculate movement direction based on camera orientation
            return forward * input.y + right * input.x;
        }
        else
        {
            // Fallback to self-relative movement if no camera found
            return new Vector3(input.x, 0f, input.y);
        }
    }

    private void OnMove(InputAction.CallbackContext context)
    {
        moveInput = context.ReadValue<Vector2>();

        bool wasMoving = isMoving;
        isMoving = moveInput != Vector2.zero;

        if (!wasMoving && isMoving)
        {
            // Player just started moving - play footstep
            footstepPlayEvent?.Post(gameObject);
        }
        else if (wasMoving && !isMoving)
        {
            // Player stopped moving - stop footstep
            footstepStopEvent?.Post(gameObject);
        }
    }

    private void OnMusicToggle(InputAction.CallbackContext context)
    {
        if (!isMusicPlaying)
        {
            musicPlayEvent?.Post(gameObject);
            isMusicPlaying = true;
        }
        else
        {
            musicStopEvent?.Post(gameObject);
            isMusicPlaying = false;
        }
    }

    private void OnIncreaseDryWet(InputAction.CallbackContext context)
    {
        dryWet = Mathf.Min(dryWet + 50f, 100f);
        Debug.Log($"Current drywet: {dryWet}");
        dryWetRTPC?.SetGlobalValue(dryWet);
    }

    private void OnDecreaseDryWet(InputAction.CallbackContext context)
    {
        dryWet = Mathf.Max(dryWet - 50f, 0f);
        Debug.Log($"Current drywet: {dryWet}");
        dryWetRTPC?.SetGlobalValue(dryWet);
    }

    private void OnLook(InputAction.CallbackContext context)
    {
        if (cameraTransform == null) return;

        Vector2 mouseDelta = context.ReadValue<Vector2>();
        float yaw = mouseDelta.x * mouseSensitivity * Time.deltaTime;
        float pitch = mouseDelta.y * mouseSensitivity * Time.deltaTime;

        // Apply yaw rotation to the player (Y axis)
        transform.Rotate(0f, yaw, 0f, Space.World);

        // Apply pitch rotation to the camera only (X axis), with clamping
        pitchAngle -= pitch;
        pitchAngle = Mathf.Clamp(pitchAngle, minPitch, maxPitch);
        cameraTransform.localRotation = Quaternion.Euler(pitchAngle, 0f, 0f);
    }

    void OnDestroy()
    {
        // Clean up input actions
        if (moveAction != null)
        {
            moveAction.performed -= OnMove;
            moveAction.canceled -= OnMove;
            moveAction.Disable();
            moveAction.Dispose();
        }

        if (lookAction != null)
        {
            lookAction.performed -= OnLook;
            lookAction.canceled -= OnLook;
            lookAction.Disable();
            lookAction.Dispose();
        }

        if (musicAction != null)
        {
            musicAction.performed -= OnMusicToggle;
            musicAction.Disable();
            musicAction.Dispose();
        }

        if (increaseDryWetAction != null)
        {
            increaseDryWetAction.performed -= OnIncreaseDryWet;
            increaseDryWetAction.Disable();
            increaseDryWetAction.Dispose();
        }

        if (decreaseDryWetAction != null)
        {
            decreaseDryWetAction.performed -= OnDecreaseDryWet;
            decreaseDryWetAction.Disable();
            decreaseDryWetAction.Dispose();
        }
    }
}